#include "ImpactAudio.h"

#include "Components/AudioComponent.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr int32 SampleRate = 48000;

	struct FClip
	{
		TArray<int16> Pcm;
		float Duration = 0.0f;
	};

	float LowpassAlpha(const float CutoffHz)
	{
		return 1.0f - FMath::Exp(-2.0f * PI * CutoffHz / SampleRate);
	}

	// The last few hundredths faded out, for clips that would otherwise stop while still sounding.
	void FadeTail(TArray<float>& Samples, const float Seconds)
	{
		const int32 Length = FMath::Min(Samples.Num(), FMath::CeilToInt(Seconds * SampleRate));
		for (int32 i = 0; i < Length; ++i)
		{
			Samples[Samples.Num() - Length + i] *= 0.5f * (1.0f + FMath::Cos(PI * static_cast<float>(i) / Length));
		}
	}

	FClip Finish(TArray<float>& Samples)
	{
		float Peak = KINDA_SMALL_NUMBER;
		for (const float Sample : Samples)
		{
			Peak = FMath::Max(Peak, FMath::Abs(Sample));
		}

		FClip Clip;
		Clip.Pcm.SetNumUninitialized(Samples.Num());
		for (int32 i = 0; i < Samples.Num(); ++i)
		{
			Clip.Pcm[i] = static_cast<int16>(FMath::Clamp(Samples[i] / Peak * 0.9f, -1.0f, 1.0f) * 32767.0f);
		}
		Clip.Duration = static_cast<float>(Samples.Num()) / SampleRate;
		return Clip;
	}

	struct FGunshotRecipe
	{
		float Duration;
		float CrackDecay;   // sharp supersonic crack
		float BodyDecay;    // muzzle blast noise
		float BodyCutoff;
		float BoomStartHz;  // low thump sweeping down
		float BoomEndHz;
		float BoomDecay;
		float TailDecay;    // reflections
		float TailGain;
		int32 Seed;
	};

	FClip SynthGunshot(const FGunshotRecipe& Recipe)
	{
		FRandomStream Random(Recipe.Seed);
		const int32 Count = FMath::CeilToInt(Recipe.Duration * SampleRate);
		TArray<float> Samples;
		Samples.SetNumZeroed(Count);

		const float BodyAlpha = LowpassAlpha(Recipe.BodyCutoff);
		const float TailAlpha = LowpassAlpha(450.0f);
		float Body = 0.0f;
		float Tail = 0.0f;
		float Phase = 0.0f;

		for (int32 i = 0; i < Count; ++i)
		{
			const float Time = static_cast<float>(i) / SampleRate;
			const float Noise = Random.FRandRange(-1.0f, 1.0f);

			Body += BodyAlpha * (Noise - Body);
			Tail += TailAlpha * (Noise - Tail);

			const float Frequency = Recipe.BoomEndHz + (Recipe.BoomStartHz - Recipe.BoomEndHz) * FMath::Exp(-Time / 0.03f);
			Phase += 2.0f * PI * Frequency / SampleRate;

			const float Crack = Noise * FMath::Exp(-Time / Recipe.CrackDecay);
			const float Blast = Body * 3.0f * FMath::Exp(-Time / Recipe.BodyDecay);
			const float Boom = FMath::Sin(Phase) * FMath::Exp(-Time / Recipe.BoomDecay);
			const float Reflections = Tail * 6.0f * Recipe.TailGain * FMath::Exp(-Time / Recipe.TailDecay);

			const float Attack = FMath::Min(1.0f, Time / 0.0004f);
			Samples[i] = FMath::Tanh((Crack * 0.8f + Blast * 0.7f + Boom * 0.9f + Reflections) * 1.6f) * Attack;
		}

		return Finish(Samples);
	}

	FClip SynthSurfaceImpact()
	{
		FRandomStream Random(77);
		const int32 Count = FMath::CeilToInt(0.16f * SampleRate);
		TArray<float> Samples;
		Samples.SetNumZeroed(Count);

		const float Alpha = LowpassAlpha(1800.0f);
		float Low = 0.0f;
		float Phase = 0.0f;
		for (int32 i = 0; i < Count; ++i)
		{
			const float Time = static_cast<float>(i) / SampleRate;
			const float Noise = Random.FRandRange(-1.0f, 1.0f);
			Low += Alpha * (Noise - Low);
			Phase += 2.0f * PI * 2400.0f / SampleRate;

			const float Click = Noise * FMath::Exp(-Time / 0.003f);
			const float Debris = (Noise - Low) * 0.35f * FMath::Exp(-Time / 0.025f);
			const float Ping = FMath::Sin(Phase) * 0.12f * FMath::Exp(-Time / 0.03f);
			Samples[i] = Click + Debris + Ping;
		}
		return Finish(Samples);
	}

	FClip SynthFleshImpact()
	{
		FRandomStream Random(31);
		const int32 Count = FMath::CeilToInt(0.2f * SampleRate);
		TArray<float> Samples;
		Samples.SetNumZeroed(Count);

		const float Alpha = LowpassAlpha(700.0f);
		float Low = 0.0f;
		float Phase = 0.0f;
		for (int32 i = 0; i < Count; ++i)
		{
			const float Time = static_cast<float>(i) / SampleRate;
			const float Noise = Random.FRandRange(-1.0f, 1.0f);
			Low += Alpha * (Noise - Low);
			Phase += 2.0f * PI * (110.0f + 90.0f * FMath::Exp(-Time / 0.02f)) / SampleRate;

			const float Thud = Low * 4.0f * FMath::Exp(-Time / 0.035f);
			const float Body = FMath::Sin(Phase) * FMath::Exp(-Time / 0.05f);
			const float Snap = Noise * 0.25f * FMath::Exp(-Time / 0.002f);
			Samples[i] = FMath::Tanh((Thud + Body + Snap) * 1.4f);
		}
		return Finish(Samples);
	}

	// Asked for: "si les balles touchent le gilet, ca doit faire un bruit de metal, et si le gilet craque aussi". A plate rings: a few
	// partials that are not multiples of each other (a plate or a bell, not a string), each fading at its own rate, in pairs a
	// hair apart so they beat. On top, the click of the strike and the dull thump of the carrier and the body behind it. A helmet
	// is smaller and stiffer: higher, shorter, less thump.
	FClip SynthArmorHit(const bool bHelmet, const int32 Seed)
	{
		FRandomStream Random(Seed);
		const float Duration = bHelmet ? 0.5f : 0.8f;
		const int32 Count = FMath::CeilToInt(Duration * SampleRate);
		TArray<float> Samples;
		Samples.SetNumZeroed(Count);

		static const float Ratio[] = {1.0f, 1.59f, 2.14f, 2.65f, 3.41f, 4.18f, 5.43f};
		static const float Level[] = {1.0f, 0.7f, 0.55f, 0.45f, 0.3f, 0.25f, 0.15f};
		static const float Decay[] = {0.22f, 0.17f, 0.13f, 0.1f, 0.075f, 0.06f, 0.045f};
		const float Base = (bHelmet ? 1150.0f : 610.0f) * Random.FRandRange(0.97f, 1.03f);
		const float Shorter = bHelmet ? 0.6f : 1.0f;
		float Phase[UE_ARRAY_COUNT(Ratio)][2] = {};
		const float LowAlpha = LowpassAlpha(500.0f);
		float Low = 0.0f;
		for (int32 i = 0; i < Count; ++i)
		{
			const float Time = static_cast<float>(i) / SampleRate;
			const float Noise = Random.FRandRange(-1.0f, 1.0f);
			Low += LowAlpha * (Noise - Low);
			float Ring = 0.0f;
			for (int32 Mode = 0; Mode < UE_ARRAY_COUNT(Ratio); ++Mode)
			{
				for (int32 Twin = 0; Twin < 2; ++Twin)
				{
					Phase[Mode][Twin] += 2.0f * PI * Base * Ratio[Mode] * (Twin == 0 ? 1.0f : 1.007f) / SampleRate;
					Ring += FMath::Sin(Phase[Mode][Twin]) * Level[Mode] * 0.5f * FMath::Exp(-Time / (Decay[Mode] * Shorter));
				}
			}
			const float Click = Noise * 1.2f * FMath::Exp(-Time / 0.0015f);
			const float Thump = Low * (bHelmet ? 0.8f : 2.5f) * FMath::Exp(-Time / 0.02f);
			const float Attack = FMath::Min(1.0f, Time / 0.0003f);
			Samples[i] = FMath::Tanh((Click + Thump + Ring * 0.9f) * 1.3f) * Attack;
		}
		FadeTail(Samples, 0.08f);
		return Finish(Samples);
	}

	// A plate that gives way does not ring: it cracks, a quick run of sharp snaps bunched at the start, a crunch, and a low thump.
	// Spent (bBroken), the run is longer, the thump heavier, and pieces rattle after it.
	FClip SynthArmorCrack(const bool bBroken, const int32 Seed)
	{
		FRandomStream Random(Seed);
		const float Duration = bBroken ? 0.6f : 0.3f;
		const int32 Count = FMath::CeilToInt(Duration * SampleRate);
		TArray<float> Samples;
		Samples.SetNumZeroed(Count);

		const int32 Snaps = bBroken ? 22 : 9;
		const float Spread = bBroken ? 0.25f : 0.12f;
		TArray<float> SnapTime, SnapLevel, SnapDecay;
		for (int32 Snap = 0; Snap < Snaps; ++Snap)
		{
			const float R = Random.FRand();
			SnapTime.Add(Spread * R * R);
			SnapLevel.Add(Random.FRandRange(0.4f, 1.0f));
			SnapDecay.Add(Random.FRandRange(0.001f, 0.003f));
		}
		const float HighAlpha = LowpassAlpha(1500.0f);
		const float CrunchAlpha = LowpassAlpha(2500.0f);
		const float RattleAlpha = LowpassAlpha(3000.0f);
		float Low = 0.0f, CrunchLow = 0.0f, RattleLow = 0.0f, Phase = 0.0f, Gate = 0.0f;
		for (int32 i = 0; i < Count; ++i)
		{
			const float Time = static_cast<float>(i) / SampleRate;
			const float Noise = Random.FRandRange(-1.0f, 1.0f);
			Low += HighAlpha * (Noise - Low);
			CrunchLow += CrunchAlpha * (Noise - CrunchLow);
			RattleLow += RattleAlpha * (Noise - RattleLow);
			const float High = Noise - Low;

			float Snapping = 0.0f;
			for (int32 Snap = 0; Snap < Snaps; ++Snap)
			{
				const float Since = Time - SnapTime[Snap];
				if (Since >= 0.0f && Since < 0.02f)
				{
					Snapping += High * SnapLevel[Snap] * FMath::Exp(-Since / SnapDecay[Snap]);
				}
			}
			const float Crunch = (Noise - CrunchLow) * 0.6f * FMath::Exp(-Time / 0.04f);
			Phase += 2.0f * PI * (70.0f + 90.0f * FMath::Exp(-Time / 0.03f)) / SampleRate;
			const float Thump = FMath::Sin(Phase) * (bBroken ? 0.8f : 0.35f) * FMath::Exp(-Time / 0.06f);
			float Rattle = 0.0f;
			if (bBroken)
			{
				if (i % 480 == 0)
				{
					Gate = Random.FRand() < 0.35f ? Random.FRandRange(0.4f, 1.0f) : 0.0f;
				}
				Rattle = (Noise - RattleLow) * 0.25f * Gate * FMath::Exp(-Time / 0.15f);
			}
			const float Attack = FMath::Min(1.0f, Time / 0.0003f);
			Samples[i] = FMath::Tanh((Snapping * 2.2f + Crunch + Thump + Rattle) * 1.2f) * Attack;
		}
		FadeTail(Samples, 0.05f);
		return Finish(Samples);
	}

	// A few takes of each, so a burst on a vest does not repeat the same sample; the pitch varies on top.
	const FClip& GetArmorHitClip(const bool bHelmet)
	{
		static const FClip Vest[2] = {SynthArmorHit(false, 53), SynthArmorHit(false, 54)};
		static const FClip Helmet[2] = {SynthArmorHit(true, 91), SynthArmorHit(true, 92)};
		return (bHelmet ? Helmet : Vest)[FMath::RandRange(0, 1)];
	}

	const FClip& GetArmorCrackClip(const bool bBroken)
	{
		static const FClip Cracks[3] = {SynthArmorCrack(false, 61), SynthArmorCrack(false, 62), SynthArmorCrack(false, 63)};
		static const FClip Ruptures[2] = {SynthArmorCrack(true, 71), SynthArmorCrack(true, 72)};
		return bBroken ? Ruptures[FMath::RandRange(0, 1)] : Cracks[FMath::RandRange(0, 2)];
	}

	void WriteWav(const FString& Path, const FClip& Clip)
	{
		TArray<uint8> Bytes;
		const auto Put32 = [&Bytes](const uint32 Value) { Bytes.Append(reinterpret_cast<const uint8*>(&Value), 4); };
		const auto Put16 = [&Bytes](const uint16 Value) { Bytes.Append(reinterpret_cast<const uint8*>(&Value), 2); };
		const uint32 DataSize = Clip.Pcm.Num() * sizeof(int16);
		Bytes.Append(reinterpret_cast<const uint8*>("RIFF"), 4);
		Put32(36 + DataSize);
		Bytes.Append(reinterpret_cast<const uint8*>("WAVEfmt "), 8);
		Put32(16);
		Put16(1);
		Put16(1);
		Put32(SampleRate);
		Put32(SampleRate * sizeof(int16));
		Put16(sizeof(int16));
		Put16(16);
		Bytes.Append(reinterpret_cast<const uint8*>("data"), 4);
		Put32(DataSize);
		Bytes.Append(reinterpret_cast<const uint8*>(Clip.Pcm.GetData()), DataSize);
		FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	const FClip& GetRifleClip()
	{
		static const FClip Clip = SynthGunshot({0.55f, 0.010f, 0.06f, 2500.0f, 95.0f, 45.0f, 0.09f, 0.22f, 0.35f, 11});
		return Clip;
	}

	const FClip& GetPistolClip()
	{
		static const FClip Clip = SynthGunshot({0.45f, 0.006f, 0.045f, 1800.0f, 140.0f, 60.0f, 0.06f, 0.18f, 0.25f, 23});
		return Clip;
	}

	const FClip& GetSurfaceClip()
	{
		static const FClip Clip = SynthSurfaceImpact();
		return Clip;
	}

	const FClip& GetFleshClip()
	{
		static const FClip Clip = SynthFleshImpact();
		return Clip;
	}

	USoundAttenuation* GetAttenuation()
	{
		static TStrongObjectPtr<USoundAttenuation> Attenuation;
		if (!Attenuation.IsValid())
		{
			Attenuation.Reset(NewObject<USoundAttenuation>(GetTransientPackage()));
			FSoundAttenuationSettings& Settings = Attenuation->Attenuation;
			Settings.bAttenuate = true;
			Settings.bSpatialize = true;
			Settings.AttenuationShape = EAttenuationShape::Sphere;
			Settings.AttenuationShapeExtents = FVector(400.0f, 0.0f, 0.0f);
			Settings.FalloffDistance = 6000.0f;
		}
		return Attenuation.Get();
	}

	void Play(UWorld* World, const FClip& Clip, const FVector& Location, const bool b2D, const float Volume, const float Pitch)
	{
		if (World == nullptr || Clip.Pcm.IsEmpty())
		{
			return;
		}

		USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(GetTransientPackage());
		Wave->SetSampleRate(SampleRate);
		Wave->NumChannels = 1;
		Wave->Duration = Clip.Duration;
		Wave->bLooping = false;
		Wave->QueueAudio(reinterpret_cast<const uint8*>(Clip.Pcm.GetData()), Clip.Pcm.Num() * sizeof(int16));

		UAudioComponent* Audio = b2D
			                         ? UGameplayStatics::SpawnSound2D(World, Wave, Volume, Pitch)
			                         : UGameplayStatics::SpawnSoundAtLocation(World, Wave, Location, FRotator::ZeroRotator, Volume, Pitch,
			                                                                  0.0f, GetAttenuation());
		if (Audio == nullptr)
		{
			return;
		}

		// A procedural wave never ends by itself: stop it once the clip has played so the component is destroyed.
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(Audio, [Audio]
		{
			Audio->Stop();
		}), Clip.Duration / Pitch + 0.05f, false);
	}
}

void ImpactAudio::PlayGunshot(UWorld* World, const EImpactWeapon Weapon, const FVector& Location, const bool bShooterIsListener)
{
	const FClip& Clip = Weapon == EImpactWeapon::Rifle ? GetRifleClip() : GetPistolClip();
	Play(World, Clip, Location, bShooterIsListener, 0.9f, FMath::FRandRange(0.96f, 1.04f));
}

void ImpactAudio::PlaySurfaceImpact(UWorld* World, const FVector& Location)
{
	Play(World, GetSurfaceClip(), Location, false, 0.6f, FMath::FRandRange(0.85f, 1.2f));
}

void ImpactAudio::PlayFleshImpact(UWorld* World, const FVector& Location)
{
	Play(World, GetFleshClip(), Location, false, 1.0f, FMath::FRandRange(0.9f, 1.1f));
}

void ImpactAudio::PlayArmorHit(UWorld* World, const FVector& Location, const bool bHelmet, const float Strength)
{
	Play(World, GetArmorHitClip(bHelmet), Location, false, FMath::Lerp(0.55f, 1.0f, FMath::Clamp(Strength, 0.0f, 1.0f)), FMath::FRandRange(0.94f, 1.06f));
}

void ImpactAudio::PlayArmorCrack(UWorld* World, const FVector& Location, const bool bBroken)
{
	Play(World, GetArmorCrackClip(bBroken), Location, false, bBroken ? 1.0f : 0.85f, FMath::FRandRange(0.95f, 1.05f));
}

void ImpactAudio::WriteArmorSoundsToDisk()
{
	const FString Folder = FPaths::ProjectSavedDir() / TEXT("_preuves_reprise/sons");
	IFileManager::Get().MakeDirectory(*Folder, true);
	WriteWav(Folder / TEXT("armure_metal_gilet.wav"), SynthArmorHit(false, 53));
	WriteWav(Folder / TEXT("armure_metal_casque.wav"), SynthArmorHit(true, 91));
	WriteWav(Folder / TEXT("armure_craque.wav"), SynthArmorCrack(false, 61));
	WriteWav(Folder / TEXT("armure_rupture.wav"), SynthArmorCrack(true, 71));
	WriteWav(Folder / TEXT("ancien_impact_surface.wav"), GetSurfaceClip());
}
