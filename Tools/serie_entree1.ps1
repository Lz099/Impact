# Entree ragdoll -> animation au sol, diagnostic (Saved/_preuves_reprise/MELANGE_ANIM_RAGDOLL.md, section entree).
$Banc = 'D:\UEProjets\Impact\Tools\banc_jeu.ps1'
foreach ($Run in 1..2) {
	& $Banc -Nom e1_partie -Test partie -Duree 60 -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeImages" | Select-Object -Last 1
	& $Banc -Nom e1_soldats -Test soldats -Duree 42 -Runs 1 -Premier $Run -Extra "-ImpactSolEntreeImages" | Select-Object -Last 1
}
"serie finie"
