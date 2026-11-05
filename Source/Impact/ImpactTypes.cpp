#include "ImpactTypes.h"

const FImpactWeaponSpec& GetImpactWeaponSpec(const EImpactWeapon Weapon)
{
	// Rifle: 5.56 mm, about 750 rounds per minute. Pistol: 9 mm, one shot per click.
	static const FImpactWeaponSpec Rifle{
		TEXT("fusil"), 0.08f, true, 0.25f, 2.5f, 0.35f, 2.0f, 4.0f, 0.45f, 0.18f, 94000.0f, 0.004f, 20000.0f
	};
	static const FImpactWeaponSpec Pistol{
		TEXT("pistolet"), 0.12f, false, 0.4f, 2.0f, 0.8f, 2.5f, 5.0f, 1.1f, 0.3f, 36000.0f, 0.008f, 10000.0f
	};
	static const FImpactWeaponSpec None{
		TEXT("aucune"), 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
	};

	switch (Weapon)
	{
	case EImpactWeapon::Rifle:
		return Rifle;
	case EImpactWeapon::Pistol:
		return Pistol;
	default:
		return None;
	}
}
