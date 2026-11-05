@echo off
rem Comme jouer.bat, mais avec la posture au sol allumee (-ImpactSolRagdoll).
rem Elle demande le noeud Slot "Ragdoll" dans AB_Als_Ragdolling, qui est en place.
rem A comparer avec jouer.bat, qui lance le meme jeu sans la posture.
start "" "D:\Unreal\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" "D:\UEProjets\Impact\Impact.uproject" /ALS/ALSExtras/Levels/L_Als_Playground -game -windowed -resx=1600 -resy=900 -nosplash -ImpactSolRagdoll
