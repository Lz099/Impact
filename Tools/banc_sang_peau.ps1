# Banc "wound" (4 passants : ventre, bras, cuisse, poitrine) avec captures temoins, avec et sans -ImpactSangPeau.
# Usage : powershell -File banc_sang_peau.ps1 -Nom <dossier> -Runs 2 [-Extra "..."]
# Journaux et captures dans Saved/_preuves_reprise/sang_peau/<Nom>/.
param(
	[string]$Nom = "sang",
	[int]$Runs = 2,
	[string]$Extra = "",
	[int]$Duree = 34
)

$Editor = "D:\Unreal\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"
$Project = "D:\UEProjets\Impact\Impact.uproject"
$Dossier = "D:\UEProjets\Impact\Saved\_preuves_reprise\sang_peau\$Nom"
New-Item -ItemType Directory -Force $Dossier | Out-Null
# Juste apres chaque tir, puis chacun des quatre a 16-19 s et a 28-31 s (taches plus grandes).
$Temoins = "3.6,5.6,7.6,9.6,16.3,17.3,18.3,19.3,28.3,29.3,30.3,31.3"

$Configs = @(@{ Nom = "temoin"; Args = @() }, @{ Nom = "peau"; Args = @("-ImpactSangPeau") })
foreach ($Run in 1..$Runs) {
	foreach ($Config in $Configs) {
		$Log = "$Dossier\$($Config.Nom)_$Run.log"
		$Witness = "D:\UEProjets\Impact\Saved\Screenshots\Witness"
		Remove-Item "$Witness\*.png" -ErrorAction SilentlyContinue
		$Arguments = @("`"$Project`"", "/ALS/ALSExtras/Levels/L_Als_Playground", "-game", "-windowed", "-resx=1280", "-resy=720",
			"-ImpactTest=wound", "-ImpactWitness=$Temoins", "-ImpactQuit=$Duree", "-nosplash", "-unattended", "-abslog=`"$Log`"") + $Config.Args
		if ($Extra -ne "") { $Arguments += $Extra.Split(" ") }
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
