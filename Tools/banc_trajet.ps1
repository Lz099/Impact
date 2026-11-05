# Banc douleur repete : temoin (sans posture au sol) et posture (-ImpactSolRagdoll), N runs chacun.
# Usage : powershell -File banc_trajet.ps1 -Nom <dossier> -Runs 3 [-Extra "-ImpactSolCouple=40"] [-Test rechute]
# Les journaux vont dans Saved/_preuves_reprise/trajet/<Nom>/.
param(
	[string]$Nom = "base",
	[int]$Runs = 3,
	[string]$Extra = "",
	[switch]$SansTemoin,
	[int]$Duree = 26,
	[string]$Test = "douleur"
)

$Editor = "D:\Unreal\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"
$Project = "D:\UEProjets\Impact\Impact.uproject"
$Dossier = "D:\UEProjets\Impact\Saved\_preuves_reprise\trajet\$Nom"
New-Item -ItemType Directory -Force $Dossier | Out-Null

$Configs = @()
if (-not $SansTemoin) { $Configs += @{ Nom = "temoin"; Args = @() } }
$PostureArgs = @("-ImpactSolRagdoll")
if ($Extra -ne "") { $PostureArgs += $Extra.Split(" ") }
$Configs += @{ Nom = "posture"; Args = $PostureArgs }

foreach ($Run in 1..$Runs) {
	foreach ($Config in $Configs) {
		$Log = "$Dossier\$($Config.Nom)_$Run.log"
		$Witness = "D:\UEProjets\Impact\Saved\Screenshots\Witness"
		Remove-Item "$Witness\*.png" -ErrorAction SilentlyContinue
		$Arguments = @("`"$Project`"", "/ALS/ALSExtras/Levels/L_Als_Playground", "-game", "-windowed", "-resx=960", "-resy=540",
			"-ImpactTest=$Test", "-ImpactQuit=$Duree", "-nosplash", "-unattended", "-abslog=`"$Log`"") + $Config.Args
		$Process = Start-Process -FilePath $Editor -ArgumentList $Arguments -PassThru
		if (-not $Process.WaitForExit(300000)) {
			# Only this bench's own process, never the others.
			Stop-Process -Id $Process.Id -Force
			"TIMEOUT $($Config.Nom) $Run"
		}
		$Captures = "$Dossier\$($Config.Nom)_$($Run)_vues"
		New-Item -ItemType Directory -Force $Captures | Out-Null
		Move-Item "$Witness\*.png" $Captures -ErrorAction SilentlyContinue
		"fini $($Config.Nom) $Run"
	}
}
