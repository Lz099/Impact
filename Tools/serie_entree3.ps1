# Entree au sol, correctif durci (pose du corps physique + attente que la pose du maillage rejoigne les corps).
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
foreach ($Run in 1..5) {
	foreach ($Test in @(@{ Nom = "partie"; Duree = 60 }, @{ Nom = "soldats"; Duree = 42 })) {
		& $Banc -Nom "e3_$($Test.Nom)_juste" -Test $Test.Nom -Duree $Test.Duree -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeImages -ImpactSolEntreeJuste" | Select-Object -Last 1
		if ($Run -le 2) {
			& $Banc -Nom "e3_$($Test.Nom)_temoin" -Test $Test.Nom -Duree $Test.Duree -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeImages" | Select-Object -Last 1
		}
	}
}
"serie finie"
