// Copyright Trace. All Rights Reserved.

#include "Audio/TraceSoundBank.h"

#include "Sound/SoundBase.h"

// One call, for one reason: GetPistolLadderResetSeconds derives its floor from the PISTOL'S OWN base
// fire interval and must read it through the same accessor the gun does. Re-typing 0.315789 here
// would be a second copy of a number spec v29 §2 is actively editing.
#include "Gameplay/TraceWeaponComponent.h"   // TraceAmmo::GetBaseFireInterval

namespace TraceSoundBankPaths
{
	// ONE spelling of each of these in the C++. Scripts/import_sounds.py holds the same three
	// strings and Trace.Audio.Report prints them, so a disagreement is visible rather than silent.
	static const TCHAR* const PackageDir = TEXT("/Game/Trace/Audio");
	static const TCHAR* const BankName   = TEXT("DA_TraceSoundBank");
	static const TCHAR* const SoundPrefix = TEXT("S_");
}

USoundBase* UTraceSoundBank::Find(FName Event) const
{
	if (const TObjectPtr<USoundBase>* Found = Events.Find(Event))
	{
		return Found->Get();
	}
	return nullptr;
}

float UTraceSoundBank::VolumeFor(FName Event) const
{
	if (const float* Found = VolumeTrim.Find(Event))
	{
		return FMath::Max(0.f, *Found);
	}
	return 1.f;
}

const TCHAR* UTraceSoundBank::DefaultBankPath()
{
	// The long form (Package.Object) so FSoftObjectPath resolves it without a second guess.
	static const FString Path = FString::Printf(TEXT("%s/%s.%s"),
		TraceSoundBankPaths::PackageDir, TraceSoundBankPaths::BankName, TraceSoundBankPaths::BankName);
	return *Path;
}

const TCHAR* UTraceSoundBank::SoundPackageDir()
{
	return TraceSoundBankPaths::PackageDir;
}

FString UTraceSoundBank::SoundAssetNameFor(FName Event)
{
	return FString::Printf(TEXT("%s%s"), TraceSoundBankPaths::SoundPrefix, *Event.ToString());
}

FString UTraceSoundBank::ConventionPathFor(FName Event)
{
	const FString AssetName = SoundAssetNameFor(Event);
	return FString::Printf(TEXT("%s/%s.%s"), TraceSoundBankPaths::PackageDir, *AssetName, *AssetName);
}

// =================================================================================================
// UTraceAudioSettings
// =================================================================================================

UTraceAudioSettings::UTraceAudioSettings()
{
	// The default bank, spelled once (above) and defaulted here. A project that wants a different
	// bank changes this in Project Settings; nothing in C++ reads a path directly.
	SoundBankPath = FSoftObjectPath(UTraceSoundBank::DefaultBankPath());
}

FName UTraceAudioSettings::GetCategoryName() const
{
	// Sits next to UTraceSettings under Project Settings > Game, which is where every other knob in
	// this project already lives.
	return FName(TEXT("Game"));
}

const UTraceAudioSettings& UTraceAudioSettings::Get()
{
	const UTraceAudioSettings* Settings = GetDefault<UTraceAudioSettings>();
	check(Settings != nullptr);
	return *Settings;
}

float UTraceAudioSettings::GetWorldFalloffDistanceUU() const
{
	// DERIVED, never stored. See the UPROPERTY comment: the falloff modifies the inner radius, so it
	// is a multiple of it and moves when it moves.
	return FMath::Max(1.f, WorldInnerRadiusUU) * FMath::Max(0.1f, WorldFalloffScale);
}

float UTraceAudioSettings::GetPistolLadderResetSeconds() const
{
	// DERIVED. See the long comment on PistolLadderResetIntervalFloor: the owner's 0.3 s is SHORTER
	// than the pistol's own 0.3158 s fire interval, so on its own it would reset the ladder on every
	// shot and §1c would be unreachable. The floor is a multiple of the gun's interval — the standing
	// rule — so it tracks any future change to the fire rate instead of breaking again silently.
	const float Literal = FMath::Max(0.f, PistolLadderResetSeconds);
	const float Floor = FMath::Max(0.f, PistolLadderResetIntervalFloor)
		* TraceAmmo::GetBaseFireInterval(ETraceEquippedWeapon::Gun);
	return FMath::Max(Literal, Floor);
}

float UTraceAudioSettings::GetFootstepBoostLinear() const
{
	// SPEC v31 §3. dB -> amplitude ratio: x = 10^(dB/20). Guarded at exactly 0 so the common case
	// ("no boost") is bit-exact 1.0 rather than 0.9999999 out of a pow(), which would put a phantom
	// gain mismatch in Trace.Audio.Loudness's third signal for no reason at all.
	if (FootstepVolumeBoostDb == 0.f)
	{
		return 1.f;
	}
	const float Clamped = FMath::Clamp(FootstepVolumeBoostDb, -24.f, 24.f);
	return FMath::Pow(10.f, Clamped / 20.f);
}

float UTraceAudioSettings::GetFootstepVolume() const
{
	// DERIVED, for exactly the reason above: the footstep knob MODIFIES the master, so it is stored
	// as a multiple of it and moves when it moves. Nothing in the project multiplies these two by
	// hand — UTraceAudioSubsystem::VolumeFor asks for this and Trace.Audio.Loudness reports it, so
	// there is one arithmetic and one place it can be wrong.
	//
	// *** SPEC v31 §3 ADDS A THIRD TERM AND IT IS APPLIED HERE, IN THE ONE DERIVATION. *** The +5 dB
	// the owner asked for is a RATIO on top of the v29 §1b base, not a replacement for it — see
	// FootstepVolumeBoostDb. Folding it in at this single site is what stops the boost from being
	// half-applied: VolumeFor, the loudness table and the walk harness all read this function, so a
	// footstep that is measured is a footstep that will be played at the same gain.
	//
	// CLAMPED AT 1.0 AFTER the boost, not before. FootstepVolumeScale is clamped to [0,1] because it
	// is a fraction of the master; the PRODUCT is clamped because the engine's volume multiplier
	// above 1 is a distortion risk, and at the shipped 0.15 x 1.7783 = 0.2667 the clamp is nowhere
	// near. It only bites if somebody dials in +17 dB, and then it is doing its job.
	const float Base = FMath::Max(0.f, MasterVolume) * FMath::Clamp(FootstepVolumeScale, 0.f, 1.f);
	return FMath::Clamp(Base * GetFootstepBoostLinear(), 0.f, 1.f);
}
