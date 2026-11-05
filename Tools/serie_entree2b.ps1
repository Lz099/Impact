# Reprise de serie_entree2.ps1 apres l'arret du 27 octobre au soir : l'essai soldats n.1 (coupe), puis les essais 2 a 5.
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
foreach ($Arm in @(@{ Nom = "temoin"; Extra = "-ImpactSolEntreeImages" }, @{ Nom = "juste"; Extra = "-ImpactSolEntreeImages -ImpactSolEntreeJuste" })) {
	& $Banc -Nom "e2_soldats_$($Arm.Nom)" -Test soldats -Duree 42 -Runs 1 -Premier 1 -Extra $Arm.Extra | Select-Object -Last 1
}
foreach ($Run in 2..5) {
	foreach ($Test in @(@{ Nom = "partie"; Duree = 60 }, @{ Nom = "soldats"; Duree = 42 })) {
		& $Banc -Nom "e2_$($Test.Nom)_temoin" -Test $Test.Nom -Duree $Test.Duree -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeImages" | Select-Object -Last 1
		& $Banc -Nom "e2_$($Test.Nom)_juste" -Test $Test.Nom -Duree $Test.Duree -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeImages -ImpactSolEntreeJuste" | Select-Object -Last 1
	}
}
"serie finie"
