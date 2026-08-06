#include "TraceSettings.h"

#include "Kismet/GameplayStatics.h"     // UGameplayStatics::ParseOption
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "Trace.h"                      // LogTraceGame
#include "TraceTypes.h"

#if WITH_EDITOR || !UE_BUILD_SHIPPING
// Live-tuning support, and the dev-only verification commands at the bottom of this file. See
// ApplyLiveMovementTuning() for why a settings object is allowed to reach into character movement
// components, and why it is limited to re-asserting exactly the two fields BeginPlay already writes.
#include "Containers/Ticker.h"          // FTSTicker, for Trace.LiveEditTest's delay
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"        // FAutoConsoleCommandWithArgs
#include "UObject/UnrealType.h"         // FProperty, for naming the edited property
#include "UObject/UObjectIterator.h"
#endif

// Why the FTraceTrailPoint replication callbacks live in *this* translation unit:
//
// The callbacks have to reach UTraceTrailComponent::OnTrailPointsChanged(). TraceTrailComponent.h
// already includes TraceTypes.h (it declares `UPROPERTY(Replicated) FTraceTrailPointArray
// TrailPoints`), so TraceTypes.h cannot include TraceTrailComponent.h back — that is a hard
// include cycle, and #pragma once would just leave one of the two headers half-parsed.
//
// The callbacks are also non-virtual, name-detected hooks that FastArrayDeltaSerialize calls
// through the *item* type, so they cannot be moved onto the component. They must exist as
// FTraceTrailPoint members with an out-of-line definition somewhere that is allowed to see the
// component's full declaration.
//
// TraceSettings.cpp is the only .cpp in this ownership slice, so it is that somewhere. The cost
// is a single compile-time edge (this .cpp -> TraceTrailComponent.h); no header gains a
// dependency, and no other file has to know about the arrangement.
#include "Gameplay/TraceTrailComponent.h"


namespace
{
	/**
	 * The session's latched bot difficulty.
	 *
	 * A free static rather than a mutable field on the CDO for one specific reason: the CDO is
	 * re-read from config on a hot reload and by the Project Settings editor, either of which would
	 * silently snap a running match back to the shipped default halfway through. The player's
	 * choice has to outlive the config system, so it lives outside it.
	 */
	struct FBotDifficultyLatch
	{
		bool bResolved = false;
		EBotDifficulty Difficulty = EBotDifficulty::Easy;
	};

	FBotDifficultyLatch& DifficultyLatch()
	{
		static FBotDifficultyLatch Latch;
		return Latch;
	}

	/** "easy" / "normal" / "hard" / "0" / "1" / "2", case- and whitespace-insensitive. */
	bool ParseBotDifficulty(const FString& Token, EBotDifficulty& OutDifficulty)
	{
		const FString Trimmed = Token.TrimStartAndEnd().ToLower();

		if (Trimmed == TEXT("easy")   || Trimmed == TEXT("0")) { OutDifficulty = EBotDifficulty::Easy;   return true; }
		if (Trimmed == TEXT("normal") || Trimmed == TEXT("1")) { OutDifficulty = EBotDifficulty::Normal; return true; }
		if (Trimmed == TEXT("hard")   || Trimmed == TEXT("2")) { OutDifficulty = EBotDifficulty::Hard;   return true; }

		return false;
	}
}

UTraceSettings::UTraceSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Scalars use in-class default initialisers (see the header) so the defaults are readable next
	// to their documentation. The three difficulty profiles are the exception: twenty-one floats in
	// a braced initialiser is unreadable and one comma away from silently shifting every value one
	// field to the left, so they are assigned by name here. Config still wins over all of it — this
	// only establishes the CDO defaults that DefaultGame.ini is then layered on top of.

	// =============================================================================================
	// HOW THESE NUMBERS WERE CHOSEN
	//
	// The previous pass set them by feel, in the direction the last playtest complained about, and
	// overshot twice: first Easy killed an idle human in under two seconds, then Easy could not kill
	// a stationary human at all — zero deaths in a full match, with the player walking the length of
	// the field and back untouched.
	//
	// So the aim error is now derived rather than guessed. A bot's shot lands inside a rectangle of
	// angular error 2E wide (yaw) by E tall (pitch; AimError.Y is halved in UpdateCombat), uniformly
	// distributed. A character capsule is roughly 38uu in radius and 90uu in half-height, so at range
	// R the target subtends about (4355/R) by (10314/R) degrees. The chance a single shot connects is
	// therefore
	//
	//     P(R, E)  ~=  2.25e7 / (R^2 * E^2)
	//
	// and the time to land the three body shots a kill needs, while a bot is actually engaged and
	// past its reaction delay, is
	//
	//     TTK  =  (3 / P) * FireInterval / DutyCycle,     DutyCycle = burst / (burst + rest)
	//
	// NOTE, FIRE RATE CHANGED THIS PASS: FireInterval went 0.12 -> 0.16 (a 25% slower gun, asked for
	// explicitly). FireInterval is a LINEAR term in the TTK above, so every measured figure quoted
	// per-profile below is now about 33% longer than it was when it was measured — for the bots AND
	// for the player, since both fire the same weapon. The three difficulties keep their ratios to
	// each other exactly, which is what these profiles exist to control; the whole ladder simply
	// moved down one notch in absolute lethality. Deliberately NOT compensated for here: silently
	// speeding the bots back up would undo half of the change that was requested.
	//
	// Each profile below states the TTK it is aiming for at its own PreferredCombatRange, and the
	// numbers are solved for that. The model is a lower bound on real time-to-kill — it assumes
	// unbroken line of sight and a stationary target — but it is what makes the three difficulties
	// separable by a ratio instead of by vibes, and the measured runs land where it says they will.
	//
	// The shipped Easy profile scored P = 2.0% per shot at its own preferred range, i.e. 150 shots
	// and around 200 seconds of unbroken engagement per kill. That is the whole "bots ignore the
	// player" complaint, and no amount of reaction or burst tuning was ever going to reach it.
	//
	// ---------------------------------------------------------------------------------------------
	// ENGAGEMENT RANGE AND THE ARENA THAT TRIPLED IN SIZE
	//
	// Every MaxEngagementRange below is also raised. These were tuned against an 8000 x 4000 field,
	// where 3000uu is most of the pitch; on 33600 x 9600 it is close quarters. Measured with the
	// player walked up the field, bots had them inside SightRange for 69.7% of bot-ticks but inside
	// MaxEngagementRange with line of sight for 16.0% — they could see the player fine and simply
	// were not allowed to shoot. Meanwhile HitscanRange is 36000 (it has to span the field's 34944uu
	// diagonal), so the player could shoot back from anywhere. That asymmetry is indistinguishable
	// from "the bots ignore me".
	//
	// Widening the envelope does NOT make bots snipe: the per-range aim error term means a shot at
	// 4200uu on Easy carries ~15 degrees of error and essentially never lands. What it buys is
	// pressure — you get shot at, and you take chip damage, well before anyone can actually kill
	// you, which is what a fight is supposed to feel like at a distance.
	// =============================================================================================

	// ---- EASY ---------------------------------------------------------------------------------
	// The brief: beatable by someone who has never played, but genuinely present — a new player must
	// take damage and must sometimes die. Target TTK ~23s of sustained engagement at 1900uu, which
	// with the ~15% of match time an average player actually spends in a bot's engagement envelope
	// works out to a death every minute or two rather than never.
	//
	// P(1900, 6.84) = 13.3% per shot -> 23 shots; duty cycle 0.28/(0.28+0.725) = 28% -> TTK 9.8s.
	//
	// The first attempt at this profile aimed for a 23s TTK and MEASURED as 0 deaths and 68 damage
	// against a player walked up the field for three minutes: real engagements are shorter, further
	// out and more often broken by cover than the model's "unbroken line of sight at the preferred
	// range" assumption, so the delivered lethality came in about four times under the modelled
	// figure. The model is still the right way to keep the three difficulties in ratio; it just
	// needed calibrating against a measured run, which is what these numbers are.
	//
	// The cross-speed term is left high (4.0 deg per 1000uu/s): at walk speed that is +2.9 degrees,
	// which roughly doubles Easy's TTK. Moving is the counterplay a new player discovers first and
	// it should pay well.
	//
	// ---- MEASURED, and then trimmed ------------------------------------------------------------
	// With the measurement harness fixed (it had been holding the walk key for one second in fifteen,
	// which is why every previous run reported a player who never reached a fight), a defenceless
	// synthetic player walked into the middle of the arena for two minutes on each profile:
	//
	//     EASY    first death 65s   1.38 kills/min   138 damage/min
	//     NORMAL  first death 20s   2.00 kills/min   200 damage/min
	//     HARD    first death 21s   2.50 kills/min   273 damage/min
	//
	// Monotonic at last, but Easy sat only 1.45x off Normal, which is not enough of a step for the
	// bottom rung of a three-rung ladder. The per-range error and the burst rest below are each
	// nudged one notch — the two cheapest levers that cost accuracy and DPS without touching how
	// quickly a bot NOTICES you, because a slow, present opponent reads as fair while an oblivious
	// one reads as broken.
	BotEasy.ReactionTimeSeconds           = 0.70f;   // was 0.95: x1.35 personality x1.45 jitter was 1.9s,
	                                                 // longer than most engagements lasted.
	BotEasy.ReactionJitterFraction        = 0.45f;
	BotEasy.ReacquireDelaySeconds         = 0.95f;   // was 1.40
	BotEasy.AimErrorDegrees               = 3.8f;    // was 6.5, then 5.0
	BotEasy.AimErrorPerThousandRange      = 1.85f;   // 3.4 -> 2.4 -> 1.6 -> 2.1 -> here; dominant term at arena ranges
	BotEasy.AimErrorPerThousandCrossSpeed = 4.0f;
	BotEasy.AimErrorMaxDegrees            = 20.f;    // was 26
	BotEasy.AimTurnRateDegrees            = 150.f;
	BotEasy.FireConeDegrees               = 3.5f;
	BotEasy.SightRange                    = 6000.f;  // was 5200
	BotEasy.MaxEngagementRange            = 4200.f;  // was 3000; see the arena-scale note above
	BotEasy.PreferredCombatRange          = 1900.f;
	BotEasy.BurstDurationMin              = 0.20f;
	BotEasy.BurstDurationMax              = 0.36f;
	BotEasy.BurstRestMin                  = 0.60f;   // 0.90 -> 0.70 -> 0.55 -> 0.65 -> here
	BotEasy.BurstRestMax                  = 0.97f;   // 1.60 -> 1.15 -> 0.90 -> 1.05 -> here
	BotEasy.DecisionInterval              = 0.30f;
	BotEasy.Aggression                    = 0.45f;
	// 2 -> 3 and 0.45 -> 0.60. PunisherCount now takes one bot out of the pressure group to hold a
	// bead on the carrier instead (spec §4), so leaving InterceptorCount at 2 quietly cut the
	// trace-hunting party from two to one on Easy — and the trace-hunt is the signature mechanic,
	// which the profile comment on InterceptorCount says explicitly must not be hidden on Easy.
	// Neither number affects how dangerous a bot is to SHOOT at: an interceptor never fires.
	BotEasy.InterceptorCount              = 3;
	BotEasy.TrailDashCommitChance         = 0.60f;
	BotEasy.PassChance                    = 0.60f;

	// Spec v2 additions. EASY IS DEFENDED HERE, and mostly by one number.
	//
	// Positional damage (head 100 / body 40 / leg 25) plus zero weapon spread makes every difficulty
	// strictly more lethal than the profile above was tuned against: a body burst is still three
	// shots, but a head hit is now an instant kill and there is no longer any movement inaccuracy to
	// bail a bot out of a marginal shot. Nine Easy bots each holding a one-shot kill would obliterate
	// the ~0.72 human deaths/minute baseline that was judged reasonable, so Easy simply does not take
	// the shot: HeadshotAimFraction is zero and its bots aim centre mass, where three connections are
	// needed and the reaction/burst model still governs.
	BotEasy.HeadshotAimFraction           = 0.f;
	BotEasy.PunisherCount                 = 1;
	BotEasy.PassCaution                   = 0.25f;   // passes into trouble and pays for it
	BotEasy.MovementTechChance            = 0.50f;

	// The aim error is opened back up a little to pay for the spread that used to exist on the
	// weapon and no longer does (spec §6: "there is no movement inaccuracy, set spread to 0").
	// Without this, Easy silently got more accurate this pass without anyone choosing that.
	BotEasy.AimErrorDegrees               = 4.4f;    // was 3.8, before spread was removed
	BotEasy.AimErrorPerThousandRange      = 2.05f;   // was 1.85, same reason

	// ---- NORMAL -------------------------------------------------------------------------------
	// Roughly a competent human. A real fight: standing in the open in front of one bot kills you in
	// about seven seconds, so trading shots is viable but holding an angle is not.
	//
	// P(1700, 5.01) = 31% per shot -> 10 shots; duty cycle 0.38/(0.38+0.61) = 38% -> TTK 3.1s.
	BotNormal.ReactionTimeSeconds           = 0.52f;
	BotNormal.ReactionJitterFraction        = 0.35f;
	BotNormal.ReacquireDelaySeconds         = 0.70f;
	BotNormal.AimErrorDegrees               = 2.8f;   // was 3.6
	BotNormal.AimErrorPerThousandRange      = 1.3f;   // was 1.8, then 2.0
	BotNormal.AimErrorPerThousandCrossSpeed = 2.2f;
	BotNormal.AimErrorMaxDegrees            = 18.f;
	BotNormal.AimTurnRateDegrees            = 250.f;
	BotNormal.FireConeDegrees               = 3.5f;
	BotNormal.SightRange                    = 6500.f;  // was 5500
	BotNormal.MaxEngagementRange            = 4800.f;  // was 3400
	BotNormal.PreferredCombatRange          = 1700.f;
	BotNormal.BurstDurationMin              = 0.26f;
	BotNormal.BurstDurationMax              = 0.50f;
	BotNormal.BurstRestMin                  = 0.42f;  // was 0.50, then 0.55
	BotNormal.BurstRestMax                  = 0.80f;  // was 0.95, then 1.05
	BotNormal.DecisionInterval              = 0.22f;
	BotNormal.Aggression                    = 0.70f;
	BotNormal.InterceptorCount              = 3;
	BotNormal.TrailDashCommitChance         = 0.75f;
	BotNormal.PassChance                    = 0.55f;

	// Normal takes the head shot about a third of the time. That is enough for a player to notice
	// that standing still is now punished much harder than it used to be, without every trade being
	// decided by one bullet.
	BotNormal.HeadshotAimFraction           = 0.30f;
	BotNormal.PunisherCount                 = 2;
	BotNormal.PassCaution                   = 0.60f;
	BotNormal.MovementTechChance            = 0.75f;

	// ---- HARD ---------------------------------------------------------------------------------
	// Punishing: you must use cover and you must keep moving. Standing in the open in front of one
	// bot kills you in under two seconds.
	//
	// The shipped values were not "hard", they were an aimbot in all but name — P(1500, 3.25) put
	// every single shot on target, so a Hard bot deleted a stationary player in 0.57s, faster than
	// the human could react to being shot at. The error is opened up enough that the player loses to
	// positioning rather than to a coin flip they were never shown.
	//
	// P(1500, 4.3) = 54% per shot -> 5.6 shots; duty cycle 0.575/(0.575+0.425) = 57.5% -> TTK 1.2s.
	BotHard.ReactionTimeSeconds           = 0.26f;
	BotHard.ReactionJitterFraction        = 0.25f;
	BotHard.ReacquireDelaySeconds         = 0.35f;
	BotHard.AimErrorDegrees               = 2.5f;    // was 1.9, then 3.0
	BotHard.AimErrorPerThousandRange      = 1.2f;    // was 0.9, then 1.5
	BotHard.AimErrorPerThousandCrossSpeed = 1.2f;
	BotHard.AimErrorMaxDegrees            = 12.f;
	BotHard.AimTurnRateDegrees            = 420.f;
	BotHard.FireConeDegrees               = 3.0f;
	BotHard.SightRange                    = 8000.f;  // was 7000
	BotHard.MaxEngagementRange            = 6000.f;  // was 5000
	BotHard.PreferredCombatRange          = 1500.f;
	BotHard.BurstDurationMin              = 0.40f;
	BotHard.BurstDurationMax              = 0.75f;
	BotHard.BurstRestMin                  = 0.30f;   // was 0.22
	BotHard.BurstRestMax                  = 0.55f;   // was 0.45
	BotHard.DecisionInterval              = 0.16f;
	BotHard.Aggression                    = 0.90f;
	// 4 -> 3. MEASURED: with 4 interceptors and 2 punishers, ranks 0-3 hunt the trace and the punisher
	// slots start at rank 4 — which needs five living teammates all inside the assignment radius at
	// once. A 260s Hard match logged 46 deaths and ZERO punisher-ticks: the hardest difficulty was the
	// only one that never once put a gun on a passing carrier, i.e. the one place the spec's risk beat
	// most needed to bite was the one place it could not. Three and two fits a five-man side.
	BotHard.InterceptorCount              = 3;
	BotHard.TrailDashCommitChance         = 0.95f;
	BotHard.PassChance                    = 0.50f;

	// Hard goes for the head most of the time, waits for a genuinely clean window before dropping
	// its own shield to pass, and puts two guns on any enemy carrier. Being caught in the open by a
	// Hard bot should be fatal; the counterplay is cover and movement, both of which still work
	// because the aim error and the finite slew rate are untouched.
	BotHard.HeadshotAimFraction           = 0.60f;
	BotHard.PunisherCount                 = 2;
	BotHard.PassCaution                   = 1.f;
	BotHard.MovementTechChance            = 1.f;
}

const UTraceSettings& UTraceSettings::Get()
{
	// The CDO always exists for a UDeveloperSettings and is kept current by the config system,
	// so this never needs a null check and never allocates.
	return *GetDefault<UTraceSettings>();
}

FName UTraceSettings::GetCategoryName() const
{
	return FName(TEXT("Game"));
}

#if WITH_EDITOR

// =================================================================================================
// Live editing during PIE
//
// The Project Settings details panel edits this class's CDO in place, and Get() returns that same
// CDO, so the ~55 call sites that read Get() at their point of use are already live: drag a slider,
// see it on the next frame, PIE still running. Nothing in this section is needed for those.
//
// This section exists for the values that CANNOT be read at the point of use because they have been
// copied into a field that engine code reads directly. There is exactly one such family today —
// UCharacterMovementComponent::MaxWalkSpeed and MaxWalkSpeedCrouched, which
// UTraceCharacterMovementComponent::BeginPlay initialises from WalkSpeed — and it happens to be the
// single most feel-critical number a designer would want to tune live.
// =================================================================================================

FTraceSettingsChanged& UTraceSettings::OnSettingsChanged()
{
	// Function-local static rather than a class member: the delegate must outlive, and be
	// independent of, the CDO. The config system reconstructs and re-populates the CDO on a hot
	// reload, which would silently drop every subscriber if the list lived on the object.
	static FTraceSettingsChanged Delegate;
	return Delegate;
}

void UTraceSettings::ApplyLiveMovementTuning()
{
	const UTraceSettings& Settings = Get();

	// Deliberately NOT named WalkSpeed: a local that shadows a class member compiles clean on clang
	// and fails the Windows build with C4458, which Unreal promotes to an error. This project has
	// already been broken that way once.
	const float NewMaxWalkSpeed = FMath::Max(1.f, Settings.WalkSpeed);

	// TObjectIterator rather than walking worlds and actors: it is one pass over the movement
	// components that actually exist, it needs no knowledge of which worlds are live, and it cannot
	// miss a pawn that is mid-spawn. This runs once per property edit, never per frame.
	for (TObjectIterator<UCharacterMovementComponent> It; It; ++It)
	{
		UCharacterMovementComponent* Movement = *It;
		if (Movement == nullptr || !IsValid(Movement) || Movement->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			continue;
		}

		// Only components in a running game world. Editor-world (preview / blueprint-editor)
		// components are not playing the game and must keep whatever their archetype says.
		const UWorld* World = Movement->GetWorld();
		if (World == nullptr || !World->IsGameWorld())
		{
			continue;
		}

		// These are exactly the two assignments UTraceCharacterMovementComponent::BeginPlay makes,
		// so re-running them can never put a pawn into a state that a freshly spawned pawn would not
		// also be in. If that initialisation ever changes, change it here too — or, better, have the
		// movement component subscribe to OnSettingsChanged() and delete this loop.
		//
		// AS OF THIS PASS THIS IS BELT AND BRACES, NOT THE ONLY MECHANISM, and that is on purpose.
		// UTraceCharacterMovementComponent::OnMovementUpdated now calls its own
		// RefreshWalkSpeedFromSettings() every frame, which writes these same two fields from the
		// same source. Two writers of one value is normally a smell; here they are idempotent and
		// agree by construction, and each covers a case the other does not — the per-frame refresh
		// catches config changes from any source (a .ini reload, a console command), while this loop
		// reaches pawns whose movement is not currently simulating, which is precisely the state a
		// paused-PIE tuning session leaves them in.
		Movement->MaxWalkSpeed = NewMaxWalkSpeed;
		Movement->MaxWalkSpeedCrouched = NewMaxWalkSpeed * 0.5f;
	}
}

void UTraceSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName ChangedName = (PropertyChangedEvent.Property != nullptr)
		? PropertyChangedEvent.Property->GetFName()
		: NAME_None;

	// Unconditional, not gated on ChangedName == "WalkSpeed". A struct-wide paste, an "import
	// defaults", or an edit routed through a container reports NAME_None or the container's name
	// rather than the leaf, and a gate would then silently skip the one case where re-pushing
	// matters most. The loop is a few hundred iterations at worst and runs on a human keystroke.
	ApplyLiveMovementTuning();

	OnSettingsChanged().Broadcast(ChangedName);

	UE_LOG(LogTraceGame, Verbose, TEXT("[TraceSettings] live edit: %s"), *ChangedName.ToString());
}

#endif // WITH_EDITOR

// ---------------------------------------------------------------------------------------------
// Bot difficulty
// ---------------------------------------------------------------------------------------------

EBotDifficulty UTraceSettings::GetBotDifficulty()
{
	const FBotDifficultyLatch& Latch = DifficultyLatch();
	return Latch.bResolved ? Latch.Difficulty : Get().BotDifficulty;
}

void UTraceSettings::SetBotDifficulty(EBotDifficulty InDifficulty)
{
	FBotDifficultyLatch& Latch = DifficultyLatch();
	Latch.Difficulty = InDifficulty;
	Latch.bResolved = true;

	UE_LOG(LogTraceGame, Display, TEXT("[BotDifficulty] %s"), BotDifficultyToString(InDifficulty));
}

void UTraceSettings::ResolveBotDifficultyFromOptions(const FString& Options, bool bForceReresolve)
{
	if (DifficultyLatch().bResolved && !bForceReresolve)
	{
		// Already answered for this match. Re-reading would let a bot that respawns after a
		// seamless travel pick up a URL that no longer describes what the player chose.
		return;
	}

	EBotDifficulty Resolved = EBotDifficulty::Easy;

	// 1. The travel URL the title screen wrote ("?difficulty=easy"). ParseOption is used rather
	//    than FParse::Value because option strings are '?'-delimited with no whitespace, so
	//    FParse::Value would happily return "easy?bots=0" as the value.
	FString Token = UGameplayStatics::ParseOption(Options, TEXT("difficulty"));

	// 2. A command-line override, so an unattended run can exercise Normal or Hard without a menu.
	if (Token.IsEmpty())
	{
		FParse::Value(FCommandLine::Get(), TEXT("difficulty="), Token);
	}

	if (!Token.IsEmpty() && ParseBotDifficulty(Token, Resolved))
	{
		SetBotDifficulty(Resolved);
		return;
	}

	// 3. Nothing said otherwise: honour the config default, which is EASY.
	SetBotDifficulty(Get().BotDifficulty);
}

const TCHAR* UTraceSettings::BotDifficultyToString(EBotDifficulty InDifficulty)
{
	switch (InDifficulty)
	{
	case EBotDifficulty::Easy: return TEXT("EASY");
	case EBotDifficulty::Hard: return TEXT("HARD");
	default:                   return TEXT("NORMAL");
	}
}

const FTraceBotProfile& UTraceSettings::GetBotProfile()
{
	const UTraceSettings& Settings = Get();

	switch (GetBotDifficulty())
	{
	case EBotDifficulty::Easy: return Settings.BotEasy;
	case EBotDifficulty::Hard: return Settings.BotHard;
	default:                   return Settings.BotNormal;
	}
}


// ---------------------------------------------------------------------------------------------
// FTraceTrailPoint replication callbacks
//
// These fire on clients only, inside FastArrayDeltaSerialize, once per changed item. They are
// deliberately trivial: they mark the owning component dirty and let it rebuild its visuals on
// its own tick.
//
// IMPORTANT for the trail component: PreReplicatedRemove runs *before* the item is erased from
// Items, and Post* callbacks can arrive several-per-packet. OnTrailPointsChanged() must
// therefore behave as "the point set changed, rebuild soon" (set a flag, rebuild in TickComponent
// or on the next frame) rather than doing an immediate full rebuild off the current contents of
// Items. Rebuilding synchronously here would both see stale data during a remove and do O(n)
// work n times per packet.
// ---------------------------------------------------------------------------------------------

void FTraceTrailPoint::PostReplicatedAdd(const FTraceTrailPointArray& Serializer)
{
	if (UTraceTrailComponent* Component = Serializer.OwnerComponent.Get())
	{
		Component->OnTrailPointsChanged();
	}
}

void FTraceTrailPoint::PostReplicatedChange(const FTraceTrailPointArray& Serializer)
{
	if (UTraceTrailComponent* Component = Serializer.OwnerComponent.Get())
	{
		Component->OnTrailPointsChanged();
	}
}

void FTraceTrailPoint::PreReplicatedRemove(const FTraceTrailPointArray& Serializer)
{
	if (UTraceTrailComponent* Component = Serializer.OwnerComponent.Get())
	{
		Component->OnTrailPointsChanged();
	}
}


#if !UE_BUILD_SHIPPING

// =================================================================================================
// VERIFICATION INSTRUMENTATION — dev builds only, compiled out of Shipping entirely.
//
// Two questions kept being answered by reading a header, which is the one place that cannot answer
// them: DefaultGame.ini is layered over the C++ initialiser at startup, so the value the game
// actually runs on is whatever the CDO holds at runtime and nothing else.
//
//   Trace.DumpSettings
//       Log the numbers UTraceSettings::Get() is returning RIGHT NOW, plus the derived values that
//       are clamped somewhere else (the trail's effective lifetime), plus the engine-owned walk
//       speed on every live movement component — which is the one family of values that is a COPY
//       and could therefore disagree with the table.
//
//   Trace.LiveEditTest <DelaySeconds> <PropertyName> <Value>
//       Drive the exact code path the Project Settings details panel drives — assign to the CDO,
//       then call PostEditChangeProperty — after a delay, so it lands mid-match rather than at
//       engine init, and dump the result. This is what "the panel retunes the game with PIE still
//       running" means, expressed as something an unattended run can prove.
//
// Neither command is wired into gameplay in any way; nothing outside this block calls them.
// =================================================================================================

namespace
{
	void DumpTraceSettings(const TCHAR* Tag)
	{
		const UTraceSettings& Table = UTraceSettings::Get();

		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] WalkSpeed=%.1f DashSpeed=%.1f DashDuration=%.3f DashCooldown=%.2f "
			     "FireInterval=%.3f TrailLifetime=%.2f (trail component uses %.2fs)"),
			Tag, Table.WalkSpeed, Table.DashSpeed, Table.DashDuration, Table.DashCooldown,
			Table.FireInterval, Table.TrailLifetime,
			UTraceTrailComponent::GetTraceLifetimeSeconds());

		UE_LOG(LogTraceGame, Display,
			// SlideImpulse and SlideExitMinSpeedFraction are gone (spec v4 §1) and so are their columns.
			// SlideMinCommitSeconds went the same way in spec v5 §3 — the slide is a one-shot ability
			// now, so there is no partial commit to report. The slide-jump is the reason to slide, so
			// it dumps on the same line.
			TEXT("[SettingsDump:%s] Slide dur=%.2f decel=%.0f maxSpeed=%.0f entryFrac=%.2f mult=%.2f "
			     "exitFrac=%.2f cooldown=%.2f exitRetention=%.2f exitCeil=%.2f | "
			     "SlideJump on=%d retain=%.2f zMul=%.2f window=%.2f windowBonus=%.2f"),
			Tag, Table.SlideDuration, Table.SlideDeceleration, Table.SlideMaxSpeed,
			Table.SlideEntrySpeedFraction, Table.SlideEntrySpeedMultiplier,
			Table.SlideExitSpeedFraction,
			Table.SlideCooldownSeconds, Table.SlideExitSpeedRetention,
			Table.SlideExitMaxSpeedMultiplier,
			Table.bSlideJumpEnabled ? 1 : 0, Table.SlideJumpHorizontalRetention, Table.SlideJumpZMultiplier,
			Table.SlideJumpWindowSeconds, Table.SlideJumpWindowSpeedBonus);

		// Spec v3 §2: the air/landing block is the new movement model, so it dumps next to the
		// slide rather than being something you have to open Project Settings to read back.
		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] Air src=%d wishCap=%.0f accel=%.0f maxAirSpeed=%.0f friction=%.2f control=%.2f | "
			     "Landing preserve=%d overspeed friction=%.2f braking=%.0f turn=%.0f | dashExit=%.2f"),
			Tag, Table.bSourceAirAcceleration ? 1 : 0, Table.AirMaxWishSpeed, Table.AirAcceleration,
			Table.MaxAirSpeed, Table.AirFriction, Table.AirControl,
			Table.bPreserveLandingMomentum ? 1 : 0, Table.GroundOverspeedFriction,
			Table.GroundOverspeedBraking, Table.GroundOverspeedTurnRate, Table.DashExitSpeedMultiplier);

		// Spec v3 §3 and §4. The pass numbers were compile-time constants in TraceCore.cpp until
		// this pass, which is precisely why "passing is inconsistent" could not be answered by
		// reading a value back at runtime.
		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] Pass hold=%.2f cooldown=%.2f cancelCooldown=%.2f range=%.0f "
			     "cone=%.1fdeg slack=%.0f grace=%.2f sticky=%.2f multiLOS=%d chestZ=%.0f | "
			     "turnoverGrace=%.2f | Parry dur=%.2f cooldown=%.2f glow=%.2f"),
			Tag, Table.PassHoldSeconds, Table.PassCooldownSeconds, Table.PassCancelCooldownSeconds,
			Table.PassMaxRange, Table.PassAimConeDegrees, Table.PassAimSlack,
			Table.PassValidationGraceSeconds, Table.PassAcquireStickySeconds,
			Table.bPassMultiPointLos ? 1 : 0, Table.PassTargetChestOffsetZ,
			Table.CoreTurnoverGraceSeconds,
			Table.ParryDuration, Table.ParryCooldown, Table.ParryGlowScale);

		// Spec v4 §5 moved three numbers that are ALSO set in DefaultGame.ini, and the ini wins. The
		// only honest way to check one of those is to read it back out of the live CDO, which is what
		// this line is for — do not answer "did the respawn time change" from a header again.
		// Note RespawnDelay here is the HUD fallback; the ENFORCED one is ATraceGameMode::RespawnDelay
		// and is pinned in DefaultGame.ini under [/Script/Trace.TraceGameMode].
		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] SPECv4 respawn=%.2f turnoverGrace=%.2f walkSpeed=%.1f | "
			     "mercyLead=%d scoringMode=%s goalWidthFrac=%.3f goalRampH=%.0f"),
			Tag, Table.RespawnDelay, Table.CoreTurnoverGraceSeconds, Table.WalkSpeed,
			Table.MercyRuleLead,
			(Table.ScoringMode == ETraceScoringMode::ThrownCoreAndGoals) ? TEXT("B-ThrownCoreAndGoals") : TEXT("A-EndzoneStatusCore"),
			Table.GoalWidthFieldFraction, Table.GoalHeightUU);

		// The slide-jump dumps on the Slide line above, not here — one number, one column.
		//
		// EVERY mode-B knob is here now, not four of eight. These are bound to ATraceCore BY NAME at
		// runtime, so a misspelled property is a silent no-op rather than a build error; this line is
		// how "is the value I typed the value being played" gets answered without a debugger. It is
		// also what caught CoreThrowUpBias, which shipped for a while as "CoreThrowUpwardBias" and
		// was dead the entire time.
		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] Ghosts spacing=%.0fuu max=%d glow=%.2f smearGlow=%.2f lod=%d | "
			     "ModeB throw=%.0f lift=%.2f grav=%.2f pickupR=%.0f lockout=%.2f cooldown=%.2f "
			     "bounce=%.2f looseReset=%.1f"),
			Tag, Table.TraceGhostSpacingUU, Table.MaxTraceGhosts, Table.TraceGhostGlow,
			Table.TraceSmearGlowScale, Table.TraceGhostForcedLOD,
			Table.CoreThrowSpeed, Table.CoreThrowUpBias, Table.CoreThrowGravityScale,
			Table.CorePickupRadius, Table.CoreThrowerPickupLockoutSeconds,
			Table.CoreThrowCooldownSeconds, Table.CoreThrowBounce,
			Table.CoreLooseResetSeconds);

		// -----------------------------------------------------------------------------------------
		// SPEC v5. Every number this pass moved or introduced, on three lines, because every one of
		// them is ALSO written in Config/DefaultGame.ini and the ini wins — so the header cannot
		// answer "what is the game actually running on" and nobody should try to read it from there
		// again. Trace.VerifyKnobs below answers the other half: whether the properties exist at all
		// for the systems that bind to them by name.
		// -----------------------------------------------------------------------------------------
		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] SPECv5 fire: FireInterval=%.3fs (= %.0f RPM) | Recoil on=%d perShot=%.2fdeg "
			     "growth=%.2f max=%.2fdeg delay=%.2fs speed=%.1f/s floor=%.1fdeg/s burstReset=%.2fs compensate=%d"),
			Tag, Table.FireInterval, (Table.FireInterval > 0.f) ? (60.f / Table.FireInterval) : 0.f,
			Table.bRecoilEnabled ? 1 : 0, Table.RecoilPitchPerShot, Table.RecoilPitchGrowthPerShot,
			Table.RecoilMaxPitchDegrees, Table.RecoilRecoveryDelaySeconds, Table.RecoilRecoverySpeed,
			Table.RecoilRecoveryMinRateDegrees, Table.RecoilBurstResetSeconds,
			Table.bRecoilPlayerCompensationCancels ? 1 : 0);

		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] SPECv5 move: AirStrafe falloff=%d soft=%.0f exp=%.2f hardCap=%d hard=%.0f (modelMax=%.0f) | "
			     "Slide(ability) dur=%.2f hiddenCooldown=%.2f | SlideJump window=%.2f speed=%.2f height=%.2f | "
			     "Mantle on=%d reach=%.0f minH=%.0f maxH=%.0f dur=%.2f upPhase=%.2f cooldown=%.2f minFwd=%.0f ledgeGrace=%.3f"),
			Tag, Table.bAirStrafeGainFalloff ? 1 : 0, Table.AirStrafeSoftCapSpeed,
			Table.AirStrafeFalloffExponent, Table.bAirStrafeHardCap ? 1 : 0,
			Table.AirStrafeHardCapSpeed, Table.MaxAirSpeed,
			Table.SlideDuration, Table.SlideCooldownSeconds,
			Table.SlideJumpWindowSeconds, Table.SlideJumpWindowSpeedBonus, Table.SlideJumpWindowZBonus,
			Table.bMantleEnabled ? 1 : 0, Table.MantleReachUU, Table.MantleMinHeightUU,
			Table.MantleMaxHeightUU, Table.MantleDurationSeconds, Table.MantleUpPhaseFraction,
			Table.MantleCooldownSeconds, Table.MantleMinForwardSpeed, Table.LedgeGroundGraceSeconds);

		// Mode B geometry and weight. The BASE throw values are printed next to the mass scale that
		// multiplies them, because the number a designer types is not the number the Core flies at —
		// ATraceCore derives gravity x M, speed / sqrt(M), bias x M^1.5, bounce / M from these.
		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] SPECv5 modeB: goalWidthFrac=%.4f (=%.0fuu ring diameter on a 9600uu field) goalRampH=%.0f | "
			     "weight M=%.2f applied to base throw=%.0f grav=%.2f bias=%.2f -> effective throw=%.0f grav=%.2f"),
			Tag, Table.GoalWidthFieldFraction, Table.GoalWidthFieldFraction * 9600.f, Table.GoalHeightUU,
			Table.CoreMassScale, Table.CoreThrowSpeed, Table.CoreThrowGravityScale, Table.CoreThrowUpBias,
			Table.CoreThrowSpeed / FMath::Sqrt(FMath::Max(0.01f, Table.CoreMassScale)),
			Table.CoreThrowGravityScale * Table.CoreMassScale);

		// Spec v4 §4. The impact sphere is deleted from ATraceTracer, so there is nothing to report
		// for it — the radii are the whole of what is left to get wrong.
		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] Tracer radiusPerLen=%.5f min=%.2f max=%.2f haloRatio=%.2f "
			     "muzzle=%d muzzleR=%.2f (impact sphere: DELETED)"),
			Tag, Table.TracerRadiusPerLength, Table.TracerRadiusMinUU, Table.TracerRadiusMaxUU,
			Table.TracerSheathRadiusRatio, Table.bTracerMuzzleFlash ? 1 : 0, Table.TracerMuzzleRadiusUU);

		// The engine-owned copies. Named MovementIt/LiveMovement rather than anything that reads
		// like a base-class member: a local shadowing one fails the Windows build (C4458).
		int32 LiveComponents = 0;
		int32 Disagreeing = 0;
		for (TObjectIterator<UCharacterMovementComponent> MovementIt; MovementIt; ++MovementIt)
		{
			UCharacterMovementComponent* LiveMovement = *MovementIt;
			if (LiveMovement == nullptr || !IsValid(LiveMovement)
				|| LiveMovement->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
			{
				continue;
			}

			const UWorld* LiveWorld = LiveMovement->GetWorld();
			if (LiveWorld == nullptr || !LiveWorld->IsGameWorld())
			{
				continue;
			}

			++LiveComponents;
			if (!FMath::IsNearlyEqual(LiveMovement->MaxWalkSpeed, Table.WalkSpeed, 0.5f))
			{
				++Disagreeing;
				UE_LOG(LogTraceGame, Display, TEXT("[SettingsDump:%s]   %s MaxWalkSpeed=%.1f (table says %.1f)"),
					Tag, *GetNameSafe(LiveMovement->GetOwner()), LiveMovement->MaxWalkSpeed, Table.WalkSpeed);
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] %d live movement components, %d disagree with WalkSpeed=%.1f"),
			Tag, LiveComponents, Disagreeing, Table.WalkSpeed);
	}

	void ApplyLiveEdit(FString PropertyName, float NewValue)
	{
		UTraceSettings* MutableTable = GetMutableDefault<UTraceSettings>();
		FProperty* Edited = UTraceSettings::StaticClass()->FindPropertyByName(FName(*PropertyName));
		FFloatProperty* EditedFloat = CastField<FFloatProperty>(Edited);

		if (MutableTable == nullptr || EditedFloat == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[LiveEdit] '%s' is not a float property of UTraceSettings."), *PropertyName);
			return;
		}

		const float PreviousValue = EditedFloat->GetPropertyValue_InContainer(MutableTable);
		EditedFloat->SetPropertyValue_InContainer(MutableTable, NewValue);

#if WITH_EDITOR
		// Byte for byte what the details panel does after it writes the value.
		FPropertyChangedEvent EditEvent(Edited, EPropertyChangeType::ValueSet);
		MutableTable->PostEditChangeProperty(EditEvent);
#endif

		UE_LOG(LogTraceGame, Display, TEXT("[LiveEdit] %s: %.3f -> %.3f (panel path%s)"),
			*PropertyName, PreviousValue, NewValue,
			WITH_EDITOR ? TEXT(", PostEditChangeProperty called") : TEXT(", no editor hook in this build"));

		DumpTraceSettings(TEXT("after-live-edit"));
	}

	// =============================================================================================
	// Trace.VerifyKnobs — "does this slider exist, and is it the type its reader expects".
	//
	// WHY THIS COMMAND EXISTS AT ALL. Several systems bind to this page BY NAME at runtime through
	// FindFProperty (ATraceCore's whole mode-B block does, and the live-edit path does), and a
	// misspelled or retyped property is NOT a build error. It is a slider on the settings panel that
	// silently moves nothing. Five of the eight mode-B knobs shipped dead exactly that way, and one
	// of them (CoreThrowUpBias, briefly declared as CoreThrowUpwardBias) was dead for a whole pass.
	//
	// So the build states its own answer. The table below is every knob this pass introduced or
	// moved, plus the by-name mode-B set, each with the FProperty type its reader will cast to. If a
	// name here is wrong the command says so by name; if a reader's name is wrong, the reader's own
	// binding log (ATraceCore's LogKnobBindings) says so. Between them there is nowhere for a dead
	// knob to hide.
	// =============================================================================================

	enum class EKnobType : uint8 { Float, Bool, Int };

	struct FKnobSpec
	{
		const TCHAR* Name;
		EKnobType Type;
		const TCHAR* Note;
	};

	void VerifyTraceKnobs()
	{
		static const FKnobSpec Knobs[] =
		{
			// --- spec v5 §5, fire rate ---------------------------------------------------------
			{ TEXT("FireInterval"),                    EKnobType::Float, TEXT("150 RPM = 0.40s") },

			// --- spec v5 §6, upwards recoil ----------------------------------------------------
			{ TEXT("bRecoilEnabled"),                  EKnobType::Bool,  TEXT("recoil master switch") },
			{ TEXT("RecoilPitchPerShot"),              EKnobType::Float, TEXT("per-shot kick") },
			{ TEXT("RecoilPitchGrowthPerShot"),        EKnobType::Float, TEXT("sustained-fire growth") },
			{ TEXT("RecoilMaxPitchDegrees"),           EKnobType::Float, TEXT("climb ceiling") },
			{ TEXT("RecoilRecoveryDelaySeconds"),      EKnobType::Float, TEXT("recovery delay") },
			{ TEXT("RecoilRecoverySpeed"),             EKnobType::Float, TEXT("recovery rate") },
			{ TEXT("RecoilRecoveryMinRateDegrees"),    EKnobType::Float, TEXT("recovery floor") },
			{ TEXT("RecoilBurstResetSeconds"),         EKnobType::Float, TEXT("burst reset gap") },
			{ TEXT("bRecoilPlayerCompensationCancels"),EKnobType::Bool,  TEXT("compensation cancels recovery") },

			// --- spec v5 §1, air-strafe cap and falloff  [ALL FOUR BOUND BY NAME by the CMC] ----
			{ TEXT("bAirStrafeGainFalloff"),           EKnobType::Bool,  TEXT("diminishing returns on/off [by-name bind]") },
			{ TEXT("AirStrafeSoftCapSpeed"),           EKnobType::Float, TEXT("where falloff begins [by-name bind]") },
			{ TEXT("AirStrafeFalloffExponent"),        EKnobType::Float, TEXT("falloff shape [by-name bind]") },
			{ TEXT("bAirStrafeHardCap"),               EKnobType::Bool,  TEXT("hard cap on/off [by-name bind]") },
			{ TEXT("AirStrafeHardCapSpeed"),           EKnobType::Float, TEXT("the hard cap [by-name bind]") },
			{ TEXT("MaxAirSpeed"),                     EKnobType::Float, TEXT("model ceiling, NOT the v5 cap") },

			// --- spec v5 §3, slide as an ability -----------------------------------------------
			{ TEXT("SlideDuration"),                   EKnobType::Float, TEXT("ability duration") },
			{ TEXT("SlideCooldownSeconds"),            EKnobType::Float, TEXT("HIDDEN cooldown - do not draw it") },
			{ TEXT("SlideJumpWindowSeconds"),          EKnobType::Float, TEXT("well-timed window") },
			{ TEXT("SlideJumpWindowSpeedBonus"),       EKnobType::Float, TEXT("well-timed speed multiplier - v8 §8: 1.25 -> 1.3125") },
			{ TEXT("SlideJumpWindowZBonus"),           EKnobType::Float, TEXT("well-timed height multiplier [by-name bind]") },

			// --- spec v5 §7, mantle  [ALL BOUND BY NAME by the CMC] -----------------------------
			{ TEXT("bMantleEnabled"),                  EKnobType::Bool,  TEXT("mantle master switch [by-name bind]") },
			{ TEXT("MantleReachUU"),                   EKnobType::Float, TEXT("forward probe [by-name bind]") },
			{ TEXT("MantleMinHeightUU"),               EKnobType::Float, TEXT("below this the step-up handles it [by-name bind]") },
			{ TEXT("MantleMaxHeightUU"),               EKnobType::Float, TEXT("tallest climbable ledge [by-name bind]") },
			{ TEXT("MantleDurationSeconds"),           EKnobType::Float, TEXT("climb duration [by-name bind]") },
			{ TEXT("MantleUpPhaseFraction"),           EKnobType::Float, TEXT("rise before the step [by-name bind]") },
			{ TEXT("MantleCooldownSeconds"),           EKnobType::Float, TEXT("gap between mantles [by-name bind]") },
			{ TEXT("MantleMinForwardSpeed"),           EKnobType::Float, TEXT("a move, not a proximity effect [by-name bind]") },
			{ TEXT("LedgeGroundGraceSeconds"),         EKnobType::Float, TEXT("the ledge desync half of §7 [by-name bind]") },

			// --- spec v5 §4, mode B geometry and weight ----------------------------------------
			{ TEXT("GoalWidthFieldFraction"),          EKnobType::Float, TEXT("2000uu goal mouth") },
			{ TEXT("GoalHeightUU"),                    EKnobType::Float, TEXT("goal APPROACH RAMP height since v6 4.3") },

			// --- spec v6 §4.1, the mode-B catch zone  [ALL THREE BOUND BY NAME by ATraceCore] ---
			{ TEXT("CoreCatchRadius"),                 EKnobType::Float, TEXT("catch magnet radius [by-name bind]") },
			{ TEXT("CoreCatchCurveStrength"),          EKnobType::Float, TEXT("catch magnet strength [by-name bind]") },
			{ TEXT("CoreCatchThrowerLockoutSeconds"),  EKnobType::Float, TEXT("thrower excluded from own zone [by-name bind]") },

			// --- spec v6 §3, the parry punish --------------------------------------------------
			{ TEXT("bParryKillsDasher"),               EKnobType::Bool,  TEXT("parry kills the dasher") },
			{ TEXT("CoreMassScale"),                   EKnobType::Float, TEXT("THE weight knob [by-name bind]") },
			{ TEXT("CoreThrowSpeed"),                  EKnobType::Float, TEXT("base, before weight [by-name bind]") },
			{ TEXT("CoreThrowGravityScale"),           EKnobType::Float, TEXT("base, before weight [by-name bind]") },

			// --- the rest of the mode-B set, all bound BY NAME by ATraceCore --------------------
			{ TEXT("CoreThrowUpBias"),                 EKnobType::Float, TEXT("[by-name bind]") },
			{ TEXT("CorePickupRadius"),                EKnobType::Float, TEXT("[by-name bind]") },
			{ TEXT("CoreThrowerPickupLockoutSeconds"), EKnobType::Float, TEXT("[by-name bind]") },
			{ TEXT("CoreLooseResetSeconds"),           EKnobType::Float, TEXT("[by-name bind]") },
			{ TEXT("CoreThrowCooldownSeconds"),        EKnobType::Float, TEXT("[by-name bind]") },
			{ TEXT("CoreThrowBounce"),                 EKnobType::Float, TEXT("[by-name bind]") },

			// --- spec v7, every knob this pass introduced --------------------------------------
			//
			// §§1-2 the trace expires by LENGTH, not time. TrailMaxLengthUU is now the number the
			// whole mechanic hangs on, and TrailLifetime survives only to derive it and to fade the
			// legacy renderer arm — a dead TrailMaxLengthUU would silently fall back to the
			// derivation and look like it worked, which is exactly the failure this table exists for.
			{ TEXT("TrailMaxLengthUU"),                EKnobType::Float, TEXT("v7 §§1-2: 1200uu, THE expiry rule") },
			{ TEXT("TrailLifetime"),                   EKnobType::Float, TEXT("v7 §1: derivation + legacy fade ONLY, no longer expiry") },
			// §3 thinner and shallower. These two are the LETHAL volume and the DRAWN volume at once.
			{ TEXT("TrailRadius"),                     EKnobType::Float, TEXT("v7 §3: 45 -> 22.5, lethal == drawn") },
			{ TEXT("TrailHeight"),                     EKnobType::Float, TEXT("v7 §3: 190 -> 63, lethal == drawn") },
			// §4 the mode-B surface rule, bound BY NAME by ATraceCore like the rest of that block.
			{ TEXT("CoreSurfaceMaxSlopeDegrees"),      EKnobType::Float, TEXT("v7 §4: floor/top vs wall, deg from up [by-name bind]") },
			// §5 the vertical ceiling a vectorized dash may hand back, as a multiple of the jump.
			{ TEXT("DashExitVerticalSpeedMultiplier"), EKnobType::Float, TEXT("v7 §5: climb ceiling on a vertical dash") },
			// §1 fallout: the bots' tail filter changed units from seconds-of-life to uu-from-tail.
			{ TEXT("BotTrailMinPointLifeRemaining"),   EKnobType::Float, TEXT("v7 §1: now x WalkSpeed = uu skipped from the tail") },

			// --- spec v8, every knob this pass introduced or moved -----------------------------
			//
			// §7 the wall jump. ALL SEVEN are resolved by name by the CMC through TraceMoveKnob, so
			// a rename in TraceSettings.h does not fail to compile — it silently reverts the whole
			// mechanic to the built-in fallback and the ini stops driving it. That is precisely the
			// failure this table exists to make loud, and it is why a brand-new mechanic's knobs are
			// listed here on the pass that introduces them rather than the pass after.
			{ TEXT("bWallJumpEnabled"),                EKnobType::Bool,  TEXT("v8 §7: wall jump master switch [by-name bind]") },
			{ TEXT("WallJumpWindowSeconds"),           EKnobType::Float, TEXT("v8 §7: contact window, the reaction input [by-name bind]") },
			{ TEXT("WallJumpSpeedRetention"),          EKnobType::Float, TEXT("v8 §7: THE request - carry, not reset [by-name bind]") },
			{ TEXT("WallJumpOutwardImpulse"),          EKnobType::Float, TEXT("v8 §7: floor for a glancing wall jump [by-name bind]") },
			{ TEXT("WallJumpVerticalMultiplier"),      EKnobType::Float, TEXT("v8 §7: vertical, x JumpZVelocity [by-name bind]") },
			{ TEXT("WallJumpMaxConsecutive"),          EKnobType::Int,   TEXT("v8 §7: THE anti-ladder cap [by-name bind]") },
			{ TEXT("WallJumpMaxNormalZ"),              EKnobType::Float, TEXT("v8 §7: wall vs ramp [by-name bind]") },

			// §4 the thrown Core inherits the thrower's velocity. Bound BY NAME by ATraceCore like
			// the rest of that block, and its own binding check names it out loud when it misses.
			{ TEXT("CoreThrowVelocityInheritance"),    EKnobType::Float, TEXT("v8 §4: launch = impulse + thrower velocity x this [by-name bind]") },

			// §3 the parry window. Not new, but it MOVED this pass (0.10 -> 0.20) and it is the one
			// number the whole mechanic is, so it is worth a line that proves the ini still drives it.
			{ TEXT("ParryDuration"),                   EKnobType::Float, TEXT("v8 §3: 0.10 -> 0.20, THE parry") },

			// §8 the slide-jump bonus moved 1.25 -> 1.3125, but it is ALREADY in this table under the
			// spec v5 §3 block above — a second row would double-count it in the bound/dead summary,
			// so the change is recorded there in the note rather than repeated here.

			// §5 the carrier's second dash charge. The spec's first suspect was "CarrierExtraDashCharges
			// may be 0 in the ini, which WINS over the header default", so the value is worth printing.
			{ TEXT("CarrierExtraDashCharges"),         EKnobType::Int,   TEXT("v8 §5: the carrier's 2nd charge - must read 1") },
		};

		const UTraceSettings& Table = UTraceSettings::Get();
		int32 BoundCount = 0;
		int32 DeadCount = 0;

		for (const FKnobSpec& Knob : Knobs)
		{
			const FName KnobName(Knob.Name);
			const FProperty* Found = UTraceSettings::StaticClass()->FindPropertyByName(KnobName);

			FString Value = TEXT("<unbound>");
			bool bTypeOk = false;

			if (Found != nullptr)
			{
				switch (Knob.Type)
				{
				case EKnobType::Float:
					if (const FFloatProperty* AsFloat = CastField<FFloatProperty>(Found))
					{
						bTypeOk = true;
						Value = FString::Printf(TEXT("%.4f"), AsFloat->GetPropertyValue_InContainer(&Table));
					}
					break;
				case EKnobType::Bool:
					if (const FBoolProperty* AsBool = CastField<FBoolProperty>(Found))
					{
						bTypeOk = true;
						Value = AsBool->GetPropertyValue_InContainer(&Table) ? TEXT("true") : TEXT("false");
					}
					break;
				case EKnobType::Int:
					if (const FIntProperty* AsInt = CastField<FIntProperty>(Found))
					{
						bTypeOk = true;
						Value = FString::Printf(TEXT("%d"), AsInt->GetPropertyValue_InContainer(&Table));
					}
					break;
				}
			}

			// A property that exists but is `config`-less would round-trip from the header and ignore
			// DefaultGame.ini, which is the same silent failure wearing a different hat.
			const bool bConfig = (Found != nullptr) && Found->HasAnyPropertyFlags(CPF_Config);
			const bool bEditable = (Found != nullptr) && Found->HasAnyPropertyFlags(CPF_Edit);

			if (bTypeOk && bConfig && bEditable)
			{
				++BoundCount;
				UE_LOG(LogTraceGame, Display, TEXT("[VerifyKnobs]   OK   %-34s = %-10s  (%s)"),
					Knob.Name, *Value, Knob.Note);
			}
			else
			{
				++DeadCount;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[VerifyKnobs]   DEAD %-34s  exists=%d rightType=%d config=%d editable=%d  (%s)"),
					Knob.Name, (Found != nullptr) ? 1 : 0, bTypeOk ? 1 : 0, bConfig ? 1 : 0,
					bEditable ? 1 : 0, Knob.Note);
			}
		}

		const int32 TotalCount = static_cast<int32>(UE_ARRAY_COUNT(Knobs));
		if (DeadCount == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[VerifyKnobs] %d of %d knobs bound: every one exists, has the type its reader casts to, "
				     "is `config` (so DefaultGame.ini reaches it) and is EditAnywhere (so the panel does)."),
				BoundCount, TotalCount);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[VerifyKnobs] %d of %d knobs bound, %d DEAD. A dead knob is a slider that moves nothing — fix the name before shipping."),
				BoundCount, TotalCount, DeadCount);
		}
	}

	FAutoConsoleCommand CmdVerifyTraceKnobs(
		TEXT("Trace.VerifyKnobs"),
		TEXT("Dev only. Check that every tunable this pass added exists on UTraceSettings with the right type, is config-backed and is panel-editable."),
		FConsoleCommandDelegate::CreateStatic(&VerifyTraceKnobs));

	FAutoConsoleCommand CmdDumpTraceSettings(
		TEXT("Trace.DumpSettings"),
		TEXT("Dev only. Log the gameplay tuning values UTraceSettings::Get() returns right now, plus every live pawn's engine-owned MaxWalkSpeed."),
		FConsoleCommandDelegate::CreateStatic(&DumpTraceSettings, TEXT("now")));

	// FAutoConsoleCommand, not FAutoConsoleCommandWithArgs: in UE 5.8 the argument-taking form is an
	// overload of FAutoConsoleCommand's constructor, and there is no ...WithArgs type.
	FAutoConsoleCommand CmdLiveEditTest(
		TEXT("Trace.LiveEditTest"),
		TEXT("Dev only. Trace.LiveEditTest <DelaySeconds> <PropertyName> <Value> - after the delay, edit the settings CDO exactly as the Project Settings panel does and report what the live pawns end up with."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			if (Args.Num() < 3)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[LiveEdit] usage: Trace.LiveEditTest <DelaySeconds> <PropertyName> <Value>"));
				return;
			}

			const float DelaySeconds = FMath::Max(0.f, FCString::Atof(*Args[0]));
			const FString PropertyName = Args[1];
			const float NewValue = FCString::Atof(*Args[2]);

			UE_LOG(LogTraceGame, Display, TEXT("[LiveEdit] scheduled: %s = %.3f in %.1fs"), *PropertyName, NewValue, DelaySeconds);

			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[PropertyName, NewValue](float /*Delta*/)
				{
					ApplyLiveEdit(PropertyName, NewValue);
					return false;   // one shot
				}), DelaySeconds);
		}));
}

#endif // !UE_BUILD_SHIPPING
