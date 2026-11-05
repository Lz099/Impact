"""Run INSIDE the existing UE 5.7 editor; never launches or closes Unreal.

Legacy FBX API follows Tools/Weapons/FullPackImport.py. This importer creates only
the seven declared meshes and seven materials under /Game/Impact/Equipment.
Existing assets are reused only when tagged with the identical generated source;
unowned or changed originals are never overwritten. Parent handles runtime fit.
"""
from pathlib import Path
import datetime
import hashlib
import json
import traceback
import unreal as u

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'ArtSource/Equipment/manifest.json'
NOTES = ROOT / 'Saved/Equipment/models_notes.md'
DEST = '/Game/Impact/Equipment'
OWNER = 'Impact.OriginalEquipment.v1'
NAMES = {'SM_VestLight','SM_VestMedium','SM_VestHeavy','SM_Helmet',
         'SM_HelmetNight','SM_HelmetThermal','SM_HelmetLamp'}
BOUNDS_TOLERANCE_CM = .03
LIB = u.EditorAssetLibrary
ASSETS = u.AssetToolsHelpers.get_asset_tools()
MATS = u.MaterialEditingLibrary


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def set_properties(obj, values):
    for key,value in values.items():
        obj.set_editor_property(key,value)


def signature(spec):
    return hashlib.sha256(json.dumps(spec,sort_keys=True).encode('utf-8')).hexdigest()


def preflight(manifest):
    require(manifest['destination']==DEST,'Unexpected destination')
    require({r['name'] for r in manifest['meshes']}==NAMES,'Expected exactly seven equipment meshes')
    require(len(manifest['meshes'])==7,'Duplicate mesh records')
    for row in manifest['meshes']:
        require(row['asset_path']==DEST+'/'+row['name'],'Unexpected mesh path')
        path=(ROOT/row['fbx']).resolve()
        require(path.is_relative_to((ROOT/'ArtSource/Equipment/FBX').resolve()),'FBX outside equipment source folder')
        require(path.is_file(),'FBX missing: '+str(path))
        require(hashlib.sha256(path.read_bytes()).hexdigest()==row['fbx_sha256'],'FBX changed after validation: '+row['name'])
        check=row['export_validation']
        require(check['identity_node_transforms'] and check['y_handedness_compensated'],'Unverified axis transform')
        require(check['raw_vertex_ue_mapping_max_error_cm']<.0001,'FBX vertex validation failed')
    expected_materials={'M_Equipment_'+k for k in ('Olive','Charcoal','Webbing','Edge','Hardware','Lens','LampGlass')}
    require({r['name'] for r in manifest['materials']}==expected_materials,'Unexpected materials')
    specs=[(r['asset_path'],r['fbx_sha256'],u.StaticMesh) for r in manifest['meshes']]
    for row in manifest['materials']:
        require(row['asset_path']==DEST+'/'+row['name'],'Unexpected material path')
        specs.append((row['asset_path'],signature(row),u.Material))
    # Check every destination before creating anything. Re-running is safe; existing
    # content of unknown provenance or a different revision is kept intact.
    for path,stamp,kind in specs:
        if LIB.does_asset_exist(path):
            asset=LIB.load_asset(path)
            require(isinstance(asset,kind),'Unexpected existing asset type: '+path)
            require(str(LIB.get_metadata_tag(asset,'EquipmentOwner'))==OWNER,
                    'Preserving pre-existing unowned asset: '+path)
            require(str(LIB.get_metadata_tag(asset,'EquipmentSourceSHA256'))==stamp,
                    'Preserving prior asset revision: '+path+'; move/archive it explicitly before importing a new revision')


def tag(asset,stamp):
    LIB.set_metadata_tag(asset,'EquipmentOwner',OWNER)
    LIB.set_metadata_tag(asset,'EquipmentSourceSHA256',stamp)
    LIB.set_metadata_tag(asset,'EquipmentAxes','X forward; Y right; Z up; centimeters')


def material(spec):
    asset=LIB.load_asset(spec['asset_path'])
    if asset:
        return asset
    asset=ASSETS.create_asset(spec['name'],DEST,u.Material,u.MaterialFactoryNew())
    require(asset is not None,'Could not create '+spec['asset_path'])
    set_properties(asset,{'blend_mode':u.BlendMode.BLEND_OPAQUE,'two_sided':False})
    color=MATS.create_material_expression(asset,u.MaterialExpressionConstant3Vector,-350,0)
    color.set_editor_property('constant',u.LinearColor(*spec['base_color_linear']))
    MATS.connect_material_property(color,'',u.MaterialProperty.MP_BASE_COLOR)
    for index,(name,prop) in enumerate((('roughness',u.MaterialProperty.MP_ROUGHNESS),('metallic',u.MaterialProperty.MP_METALLIC))):
        node=MATS.create_material_expression(asset,u.MaterialExpressionConstant,-350,140+index*140)
        node.set_editor_property('r',spec[name])
        MATS.connect_material_property(node,'',prop)
    MATS.recompile_material(asset)
    tag(asset,signature(spec))
    return asset


def import_mesh(row):
    existing=LIB.load_asset(row['asset_path'])
    if existing:
        return existing
    options=u.FbxImportUI()
    set_properties(options,{
        'import_mesh':True,'import_as_skeletal':False,
        'mesh_type_to_import':u.FBXImportType.FBXIT_STATIC_MESH,
        'automated_import_should_detect_type':False,'import_materials':False,
        'import_textures':False,'import_animations':False})
    set_properties(options.static_mesh_import_data,{
        'combine_meshes':True,'convert_scene':False,'force_front_x_axis':False,
        'convert_scene_unit':True,'import_uniform_scale':1.,
        'import_translation':u.Vector(0,0,0),'import_rotation':u.Rotator(0,0,0),
        'auto_generate_collision':False,'generate_lightmap_u_vs':True,
        'transform_vertex_to_absolute':True,'bake_pivot_in_vertex':False,
        'build_nanite':False,'import_mesh_lods':False,
        'normal_import_method':u.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS_AND_TANGENTS,
        'coordinate_system_policy':u.CoordinateSystemPolicy.KEEP_XYZ_AXES})
    task=u.AssetImportTask()
    set_properties(task,{'filename':str(ROOT/row['fbx']),
        'destination_path':DEST,'destination_name':row['name'],
        'automated':True,'replace_existing':False,'save':False,
        'options':options,'factory':u.FbxFactory()})
    ASSETS.import_asset_tasks([task])
    mesh=LIB.load_asset(row['asset_path'])
    require(isinstance(mesh,u.StaticMesh),'Import failed: '+row['asset_path'])
    tag(mesh,row['fbx_sha256'])
    return mesh


def prepare_and_validate(mesh,row,materials,editor):
    expected=row['material_slots']
    slots=mesh.get_editor_property('static_materials')
    require(len(slots)==len(expected),'Material slot count mismatch: '+row['name'])
    actual_names=[]
    for i,slot in enumerate(slots):
        # Exact names prevent an accidental slot reorder silently swapping surfaces.
        name=str(slot.material_slot_name)
        require(name in expected and name in materials,'Unrecognized material slot '+name)
        actual_names.append(name)
        mesh.set_material(i,materials[name])
    require(set(actual_names)==set(expected),'Duplicated or missing material slots')
    editor.remove_collisions(mesh)
    body=mesh.get_editor_property('body_setup')
    require(body is not None,'Missing BodySetup: '+row['name'])
    instance=body.get_editor_property('default_instance')
    set_properties(instance,{'collision_profile_name':'NoCollision',
                             'collision_enabled':u.CollisionEnabled.NO_COLLISION,
                             'simulate_physics':False,'enable_gravity':False})
    body.set_editor_property('default_instance',instance)
    body.set_editor_property('never_needs_cooked_collision_data',True)
    for lod in range(editor.get_lod_count(mesh)):
        for section in range(mesh.get_num_sections(lod)):
            editor.enable_section_collision(mesh,False,lod,section)
            require(not editor.is_section_collision_enabled(mesh,lod,section),'Section collision remains enabled')
    require(editor.get_simple_collision_count(mesh)==0,'Unexpected collision geometry')
    require(body.get_editor_property('default_instance').get_editor_property('collision_enabled')==u.CollisionEnabled.NO_COLLISION,
            'Body collision remains enabled')
    box=mesh.get_bounding_box()
    actual=[[box.min.x,box.min.y,box.min.z],[box.max.x,box.max.y,box.max.z]]
    error=max(abs(actual[edge][axis]-row['bounds_cm'][edge][axis]) for edge in range(2) for axis in range(3))
    require(error<=BOUNDS_TOLERANCE_CM,f"Bounds mismatch {row['name']}: {actual}; max error {error:.5f} cm")
    if row['name']=='SM_HelmetLamp':
        require(box.min.y>10,'Lamp is not on helmet right/Y+ side')
    return {'asset':row['object_path'],'bounds_cm':actual,'max_error_cm':error,'simple_collision_count':0}


def write_result(report):
    NOTES.parent.mkdir(parents=True,exist_ok=True)
    before=NOTES.read_text(encoding='utf-8') if NOTES.exists() else '# Equipment models\n'
    marker='\n<!-- EQUIPMENT_IMPORT_RESULT -->\n'
    before=before.split(marker)[0]
    block='\n## Last Unreal import result\n\n```json\n'+json.dumps(report,indent=2)+'\n```\n'
    NOTES.write_text(before+marker+block,encoding='utf-8')


def main():
    report={'timestamp':datetime.datetime.now().isoformat(),'success':False,'validated':[], 'saved':[], 'error':None}
    cvar='Interchange.FeatureFlags.Import.FBX'
    previous=None
    try:
        manifest=json.loads(SOURCE.read_text(encoding='utf-8'))
        preflight(manifest)
        previous=u.SystemLibrary.get_console_variable_int_value(cvar)
        u.SystemLibrary.execute_console_command(None,cvar+' 0')
        materials={r['name']:material(r) for r in manifest['materials']}
        editor=u.get_editor_subsystem(u.StaticMeshEditorSubsystem)
        meshes=[]
        for row in manifest['meshes']:
            mesh=import_mesh(row)
            report['validated'].append(prepare_and_validate(mesh,row,materials,editor))
            meshes.append(mesh)
        require(len(meshes)==7,'Expected seven validated meshes')
        # Nothing is explicitly saved until all seven bounds/collision checks pass.
        for asset in list(materials.values())+meshes:
            require(LIB.save_loaded_asset(asset,only_if_is_dirty=False),'Save failed: '+asset.get_path_name())
            report['saved'].append(asset.get_path_name())
        report['success']=True
        u.log('Equipment: seven static meshes and seven materials validated and saved.')
    except Exception:
        report['error']=traceback.format_exc()
        u.log_error(report['error'])
        raise
    finally:
        if previous is not None:
            u.SystemLibrary.execute_console_command(None,cvar+' '+str(previous))
        write_result(report)


if __name__=='__main__':
    main()
