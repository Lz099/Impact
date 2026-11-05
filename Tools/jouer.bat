@echo off
rem Lance le jeu dans la carte de demonstration d'ALS, sans banc de test.
rem Un fichier .bat plutot qu'une ligne de commande : le chemin de carte "/ALS/..."
rem est transforme en chemin Windows par Git Bash, ce qui fait echouer le chargement.
start "" "D:\Unreal\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" "D:\UEProjets\Impact\Impact.uproject" /ALS/ALSExtras/Levels/L_Als_Playground -game -windowed -resx=1600 -resy=900 -nosplash
