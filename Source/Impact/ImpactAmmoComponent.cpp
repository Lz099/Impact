#include "ImpactAmmoComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ImpactAmmoComponent)

UImpactAmmoComponent::UImpactAmmoComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UImpactAmmoComponent::TickComponent(const float DeltaTime, const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AdvanceReload(DeltaTime);
}

UImpactAmmoComponent::FWeaponAmmo* UImpactAmmoComponent::FindAmmo(const EImpactWeapon Weapon)
{
	switch (Weapon)
	{
	case EImpactWeapon::Rifle: return &Rifle;
	case EImpactWeapon::Pistol: return &Pistol;
	default: return nullptr;
	}
}

const UImpactAmmoComponent::FWeaponAmmo* UImpactAmmoComponent::FindAmmo(const EImpactWeapon Weapon) const
{
	switch (Weapon)
	{
	case EImpactWeapon::Rifle: return &Rifle;
	case EImpactWeapon::Pistol: return &Pistol;
	default: return nullptr;
	}
}

bool UImpactAmmoComponent::CanShoot(const EImpactWeapon Weapon) const
{
	const FWeaponAmmo* Ammo = FindAmmo(Weapon);
	return !IsReloading() && Ammo != nullptr && Ammo->Magazine > 0;
}

bool UImpactAmmoComponent::ConsumeRound(const EImpactWeapon Weapon)
{
	if (!CanShoot(Weapon))
	{
		return false;
	}
	--FindAmmo(Weapon)->Magazine;
	return true;
}

bool UImpactAmmoComponent::BeginReload(const EImpactWeapon Weapon)
{
	const FWeaponAmmo* Ammo = FindAmmo(Weapon);
	if (IsReloading() || Ammo == nullptr || Ammo->Reserve <= 0 || Ammo->Magazine >= Ammo->Capacity)
	{
		return false;
	}
	ReloadWeapon = Weapon;
	ReloadElapsed = 0.0f;
	return true;
}

void UImpactAmmoComponent::CancelReload()
{
	ReloadWeapon = EImpactWeapon::None;
	ReloadElapsed = 0.0f;
}

float UImpactAmmoComponent::GetReloadDuration() const
{
	const FWeaponAmmo* Ammo = FindAmmo(ReloadWeapon);
	return Ammo != nullptr ? Ammo->ReloadDuration : 0.0f;
}

float UImpactAmmoComponent::GetReloadProgress() const
{
	const float Duration = GetReloadDuration();
	return Duration > 0.0f ? FMath::Clamp(ReloadElapsed / Duration, 0.0f, 1.0f) : 0.0f;
}

int32 UImpactAmmoComponent::GetMagazine(const EImpactWeapon Weapon) const
{
	const FWeaponAmmo* Ammo = FindAmmo(Weapon);
	return Ammo != nullptr ? Ammo->Magazine : 0;
}

int32 UImpactAmmoComponent::GetReserve(const EImpactWeapon Weapon) const
{
	const FWeaponAmmo* Ammo = FindAmmo(Weapon);
	return Ammo != nullptr ? Ammo->Reserve : 0;
}

void UImpactAmmoComponent::SetRifleCapacity(const int32 Capacity)
{
	Rifle.Capacity = FMath::Max(0, Capacity);
	const int32 Overflow = FMath::Max(0, Rifle.Magazine - Rifle.Capacity);
	Rifle.Magazine -= Overflow;
	Rifle.Reserve += Overflow;
}

void UImpactAmmoComponent::SetRifleReloadDuration(float Seconds)
{
    if (!IsReloading() && FMath::IsFinite(Seconds)) Rifle.ReloadDuration=FMath::Clamp(Seconds,1.f,6.f);
}

void UImpactAmmoComponent::AdvanceReload(const float DeltaTime)
{
	if (!IsReloading() || !FMath::IsFinite(DeltaTime) || DeltaTime <= 0.0f)
	{
		return;
	}

	FWeaponAmmo* Ammo = FindAmmo(ReloadWeapon);
	// Compare remaining time before adding, so even a huge finite frame cannot overflow.
	if (DeltaTime < Ammo->ReloadDuration - ReloadElapsed)
	{
		ReloadElapsed += DeltaTime;
		return;
	}

	// Read current capacity here: resizing while reloading cannot create, lose or overfill ammo.
	const int32 Transfer = FMath::Min(Ammo->Capacity - Ammo->Magazine, Ammo->Reserve);
	Ammo->Magazine += Transfer;
	Ammo->Reserve -= Transfer;
	CancelReload();
}
