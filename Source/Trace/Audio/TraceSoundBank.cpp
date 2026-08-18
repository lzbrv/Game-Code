// Copyright Trace. All Rights Reserved.

#include "Audio/TraceSoundBank.h"

#include "Sound/SoundBase.h"

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
