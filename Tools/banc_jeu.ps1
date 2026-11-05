# Banc repete DANS LA CONFIGURATION DE JEU : les drapeaux sont relus dans jouer_nouveau.bat a chaque lancement,
# pour que la mesure porte sur le jeu tel qu'il se joue et pas sur le monde des bancs (ETAT_AU_25_OCTOBRE.md, section 4).
# Usage : & 'Tools\banc_jeu.ps1' -Nom <dossier> -Test fuite -Duree 14 -Runs 5 [-Extra "-ImpactX -ImpactY=2"] [-Sans "-ImpactZ"] [-Premier 3]
#   (pas de "powershell -ExecutionPolicy Bypass" : refuse par le controle de securite ; appeler le script directement)
#   -Extra ajoute des drapeaux, -Sans en retire (pour un temoin avec/sans).
# Les journaux vont dans Saved/_preuves_reprise/<Nom>/<Test>_<n>.log, les images temoins dans <Test>_<n>_vues/.
param(
	[string]$Nom = "base",
	[string]$Test = "fuite",
	[int]$Duree = 14,
	[int]$Runs = 3,
	[string]$Extra = "",
	[string]$Sans = "",
	[int]$Premier = 1
)

$Editor = "D:\Unreal\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"
$Project = "D:\UEProjets\Impact\Impact.uproject"
$Dossier = "D:\UEProjets\Impact\Saved\_preuves_reprise\$Nom"
New-Item -ItemType Directory -Force $Dossier | Out-Null

# Les drapeaux du jeu : la ligne "start" de jouer_nouveau.bat, tous les -Impact... qu'elle porte.
$Ligne = (Get-Content "D:\UEProjets\Impact\Tools\jouer_nouveau.bat" | Where-Object { $_ -like 'start *' } | Select-Object -Last 1)
$Jeu = [regex]::Matches($Ligne, '-Impact[^\s"]*') | ForEach-Object { $_.Value }
if ($Sans -ne "") { $Retires = $Sans.Split(" ", [StringSplitOptions]::RemoveEmptyEntries); $Jeu = $Jeu | Where-Object { $Retires -notcontains $_ } }
$Ajouts = @()
if ($Extra -ne "") { $Ajouts = $Extra.Split(" ", [StringSplitOptions]::RemoveEmptyEntries) }
"drapeaux du jeu : $($Jeu.Count) ; ajoutes : $($Ajouts -join ' ') ; retires : $Sans" | Tee-Object "$Dossier\drapeaux.txt"

$Witness = "D:\UEProjets\Impact\Saved\Screenshots\Witness"
foreach ($Run in $Premier..($Premier + $Runs - 1)) {
	$Log = "$Dossier\$($Test)_$Run.log"
	Remove-Item "$Witness\*.png" -ErrorAction SilentlyContinue
	$Arguments = @("`"$Project`"", "/ALS/ALSExtras/Levels/L_Als_Playground", "-game", "-windowed", "-resx=1280", "-resy=720",
		"-nosplash", "-unattended", "-ImpactTest=$Test", "-ImpactQuit=$Duree", "-abslog=`"$Log`"") + $Jeu + $Ajouts
	$Process = Start-Process -FilePath $Editor -ArgumentList $Arguments -PassThru
	if (-not $Process.WaitForExit(300000)) {
		# Seulement le processus de ce banc, jamais les autres.
		Stop-Process -Id $Process.Id -Force
		"TIMEOUT $Test $Run"
	}
	$Captures = "$Dossier\$($Test)_$($Run)_vues"
	if (Get-ChildItem "$Witness\*.png" -ErrorAction SilentlyContinue) {
		New-Item -ItemType Directory -Force $Captures | Out-Null
		Move-Item "$Witness\*.png" $Captures -ErrorAction SilentlyContinue
	}
	"fini $Test $Run"
}
