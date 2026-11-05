# Entree au sol corrigee, detail image par image de ce qui est dessine ([sol-entree-vu-img]) pour trouver le saut restant.
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
foreach ($Run in 1..3) {
	& $Banc -Nom "e6_partie" -Test partie -Duree 60 -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeJuste -ImpactSolEntreeImages" | Select-Object -Last 1
	& $Banc -Nom "e6_soldats" -Test soldats -Duree 42 -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeJuste -ImpactSolEntreeImages" | Select-Object -Last 1
}
"serie finie"
