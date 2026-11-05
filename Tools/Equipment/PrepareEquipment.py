"""Run in an isolated editor instance; preserve the user's editor session."""
from pathlib import Path
import runpy, json, traceback, unreal
root=Path('D:/UEProjets/Impact')
result={'success':False}
try:
    runpy.run_path(str(root/'Tools/Equipment/MakeVisionMaterials.py'),run_name='__main__')
    runpy.run_path(str(root/'Tools/Equipment/ImportEquipment.py'),run_name='__main__')
    result['success']=True
except Exception:
    result['error']=traceback.format_exc()
    unreal.log_error(result['error'])
finally:
    (root/'Saved/Equipment/preparation.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    unreal.SystemLibrary.quit_editor()
