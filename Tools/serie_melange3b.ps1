# Troisieme serie bis : les memes vues sans le tir des soldats sur le joueur (le voile rouge "Tu es mort" cachait la moitie des images).
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
foreach ($Config in @(
	@{ Nom = "m3b_film_temoin"; Extra = "-ImpactFilmerBalle" },
	@{ Nom = "m3b_film_elan"; Extra = "-ImpactFilmerBalle -ImpactElanGarde" },
	@{ Nom = "m3b_film_fondu35"; Extra = "-ImpactFilmerBalle -ImpactElanGarde -ImpactFonduImpact=0.35" },
	@{ Nom = "m3b_film_sanspose"; Extra = "-ImpactFilmerBalle -ImpactElanGarde -ImpactNoPose" })) {
	& $Banc -Nom $Config.Nom -Test fuite -Duree 14 -Runs 2 -Extra $Config.Extra -Sans "-ImpactPNJTirent" | Select-Object -Last 1
}
"serie finie"
