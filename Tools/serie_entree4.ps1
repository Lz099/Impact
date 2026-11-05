# Entree au sol, correctif a la racine (maillage qui reste pres du corps, corps remis apres l'entree, pas de pose forcee),
# temoin et correctif en alternance. Dossiers e4_<banc>_<version>.
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
foreach ($Run in 1..6) {
	foreach ($Test in @(@{ Nom = "partie"; Duree = 60 }, @{ Nom = "soldats"; Duree = 42 })) {
		& $Banc -Nom "e4_$($Test.Nom)_temoin" -Test $Test.Nom -Duree $Test.Duree -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeImages" | Select-Object -Last 1
		& $Banc -Nom "e4_$($Test.Nom)_juste" -Test $Test.Nom -Duree $Test.Duree -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeImages -ImpactSolEntreeJuste" | Select-Object -Last 1
	}
}
"serie finie"
