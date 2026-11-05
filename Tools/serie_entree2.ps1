# Entree ragdoll -> animation au sol : temoin contre -ImpactSolEntreeJuste, en alternance (MELANGE_ANIM_RAGDOLL.md).
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
foreach ($Run in 1..5) {
	foreach ($Test in @(@{ Nom = "partie"; Duree = 60 }, @{ Nom = "soldats"; Duree = 42 })) {
		& $Banc -Nom "e2_$($Test.Nom)_temoin" -Test $Test.Nom -Duree $Test.Duree -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeImages" | Select-Object -Last 1
		& $Banc -Nom "e2_$($Test.Nom)_juste" -Test $Test.Nom -Duree $Test.Duree -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeImages -ImpactSolEntreeJuste" | Select-Object -Last 1
	}
}
"serie finie"
