# Serie du 27 octobre (Saved/_preuves_reprise/MELANGE_ANIM_RAGDOLL.md) : temoin et correctifs en alternance,
# pour qu'une derive de la machine pese pareil sur les deux. Les journaux vont dans Saved/_preuves_reprise/melange_<config>/.
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
$Tout = "-ImpactElanGarde -ImpactImpactInertiel -ImpactChuteElan -ImpactChuteFinie -ImpactSolEnchaine"
$Series = @(
	@{ Test = "fuite"; Duree = 14; Runs = 5; Configs = @(
		@{ Nom = "melange_fuite_temoin"; Extra = "" },
		@{ Nom = "melange_fuite_elan"; Extra = "-ImpactElanGarde" },
		@{ Nom = "melange_fuite_elan_inertiel"; Extra = "-ImpactElanGarde -ImpactImpactInertiel" }) },
	@{ Test = "partie"; Duree = 60; Runs = 3; Configs = @(
		@{ Nom = "melange_partie_temoin"; Extra = "" },
		@{ Nom = "melange_partie_tout"; Extra = $Tout }) },
	@{ Test = "soldats"; Duree = 42; Runs = 3; Configs = @(
		@{ Nom = "melange_soldats_temoin"; Extra = "" },
		@{ Nom = "melange_soldats_tout"; Extra = $Tout }) },
	@{ Test = "douleur"; Duree = 26; Runs = 3; Configs = @(
		@{ Nom = "melange_douleur_temoin"; Extra = "" },
		@{ Nom = "melange_douleur_tout"; Extra = $Tout }) }
)
foreach ($Serie in $Series) {
	foreach ($Run in 1..$Serie.Runs) {
		foreach ($Config in $Serie.Configs) {
			& $Banc -Nom $Config.Nom -Test $Serie.Test -Duree $Serie.Duree -Runs 1 -Premier $Run -Extra $Config.Extra | Select-Object -Last 1
		}
	}
}
"serie finie"
