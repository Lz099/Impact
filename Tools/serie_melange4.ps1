# Quatrieme serie du 27 octobre : confirmation de la regle "pas de prise d'impact en pleine course", puis les 20 tests.
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
foreach ($Run in 1..5) {
	& $Banc -Nom m4_fuite_temoin -Test fuite -Duree 14 -Runs 1 -Premier $Run -Extra "" | Select-Object -Last 1
	& $Banc -Nom m4_fuite_course -Test fuite -Duree 14 -Runs 1 -Premier $Run -Extra "-ImpactElanGarde -ImpactCourseSansPose" | Select-Object -Last 1
}
$Log = "D:\UEProjets\Impact\Saved\_preuves_reprise\m4_tests.log"
$Process = Start-Process -FilePath "D:\Unreal\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" -ArgumentList @(
	"D:\UEProjets\Impact\Impact.uproject", "-ExecCmds=`"Automation RunTests Impact`"", "-unattended", "-nopause", "-nosplash", "-NullRHI",
	"-testexit=`"Automation Test Queue Empty`"", "-abslog=`"$Log`"") -PassThru
if (-not $Process.WaitForExit(900000)) { Stop-Process -Id $Process.Id -Force; "TIMEOUT tests" }
"serie finie"
