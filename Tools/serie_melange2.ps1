# Deuxieme serie du 27 octobre, sur le binaire corrige apres relecture (Saved/_preuves_reprise/MELANGE_ANIM_RAGDOLL.md).
# Temoin et variantes en alternance. Journaux dans Saved/_preuves_reprise/m2_<config>/.
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
$Tout = "-ImpactElanGarde -ImpactImpactInertiel -ImpactChuteElan -ImpactChuteFinie -ImpactSolEnchaine"
$Series = @(
	@{ Test = "fuite"; Duree = 14; Runs = 4; Configs = @(
		@{ Nom = "m2_fuite_temoin"; Extra = "" },
		@{ Nom = "m2_fuite_elan"; Extra = "-ImpactElanGarde" },
		@{ Nom = "m2_fuite_inertiel"; Extra = "-ImpactElanGarde -ImpactImpactInertiel" },
		@{ Nom = "m2_fuite_inertiel35"; Extra = "-ImpactElanGarde -ImpactImpactInertiel -ImpactFonduInertiel=0.35" },
		@{ Nom = "m2_fuite_fondu35"; Extra = "-ImpactElanGarde -ImpactFonduImpact=0.35" },
		@{ Nom = "m2_fuite_sanspose"; Extra = "-ImpactElanGarde -ImpactNoPose" }) },
	@{ Test = "partie"; Duree = 60; Runs = 3; Configs = @(
		@{ Nom = "m2_partie_temoin"; Extra = "" },
		@{ Nom = "m2_partie_tout"; Extra = $Tout }) },
	@{ Test = "soldats"; Duree = 42; Runs = 3; Configs = @(
		@{ Nom = "m2_soldats_temoin"; Extra = "" },
		@{ Nom = "m2_soldats_tout"; Extra = $Tout }) },
	@{ Test = "douleur"; Duree = 26; Runs = 3; Configs = @(
		@{ Nom = "m2_douleur_temoin"; Extra = "" },
		@{ Nom = "m2_douleur_tout"; Extra = $Tout }) }
)
foreach ($Serie in $Series) {
	foreach ($Run in 1..$Serie.Runs) {
		foreach ($Config in $Serie.Configs) {
			& $Banc -Nom $Config.Nom -Test $Serie.Test -Duree $Serie.Duree -Runs 1 -Premier $Run -Extra $Config.Extra | Select-Object -Last 1
		}
	}
}
"serie finie"
