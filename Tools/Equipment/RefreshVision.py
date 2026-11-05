import runpy, unreal
try:
    runpy.run_path('D:/UEProjets/Impact/Tools/Equipment/MakeVisionMaterials.py',run_name='__main__')
finally:
    unreal.SystemLibrary.quit_editor()
