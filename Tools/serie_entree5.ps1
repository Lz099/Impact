# Entree au sol mesuree telle qu'elle est dessinee ([sol-entree-vu]) et guidage de la chute ([chute-melee] contre la vraie trajectoire).
# Trois versions en alternance : temoin, -ImpactSolEntreeJuste, et + -ImpactChuteGuideJuste. Dossiers e5_<banc>_<version>.
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
$Arms = @(
	@{ Nom = "temoin"; Extra = "" },
	@{ Nom = "juste"; Extra = "-ImpactSolEntreeJuste" },
	@{ Nom = "guide"; Extra = "-ImpactSolEntreeJuste -ImpactChuteGuideJuste" })
foreach ($Run in 1..5) {
	foreach ($Test in @(@{ Nom = "partie"; Duree = 60 }, @{ Nom = "soldats"; Duree = 42 })) {
		foreach ($Arm in $Arms) {
			& $Banc -Nom "e5_$($Test.Nom)_$($Arm.Nom)" -Test $Test.Nom -Duree $Test.Duree -Runs 1 -Premier $Run -Extra $Arm.Extra | Select-Object -Last 1
		}
	}
}
"serie finie"
