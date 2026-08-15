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

		// Matches the shipped config default (spec v9 §9: Easy -> Normal). Never actually read while
		// bResolved is false — GetBotDifficulty() falls back to the CDO, not to this — but a latch
		// whose idle value disagrees with the shipped default is a trap for the next reader.
		EBotDifficulty Difficulty = EBotDifficulty::Normal;
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

	EBotDifficulty Resolved = EBotDifficulty::Normal;

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

	// 3. Nothing said otherwise: honour the config default, which is NORMAL as of spec v9 §9 (it is
	//    pinned in Config/DefaultGame.ini as well as in the header, and the ini wins).
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
			     "Ledges ledgeGrace=%.3f (v12 §5: MANTLE REMOVED - no mantle knobs remain to print)"),
			Tag, Table.bAirStrafeGainFalloff ? 1 : 0, Table.AirStrafeSoftCapSpeed,
			Table.AirStrafeFalloffExponent, Table.bAirStrafeHardCap ? 1 : 0,
			Table.AirStrafeHardCapSpeed, Table.MaxAirSpeed,
			Table.SlideDuration, Table.SlideCooldownSeconds,
			Table.SlideJumpWindowSeconds, Table.SlideJumpWindowSpeedBonus, Table.SlideJumpWindowZBonus,
			Table.LedgeGroundGraceSeconds);

		// -----------------------------------------------------------------------------------------
		// SPEC v9 §§5-8. Every movement change this pass is a SCALE OVER A BASE, so the line above
		// (which prints the bases) is only half the story — 950 on the soft-cap line is correct and
		// 1045 on it would be the bug. This line prints base, scale and PRODUCT for each one, because
		// the product is the number the game is actually played at and it appears nowhere else.
		//
		// The double application this catches is not hypothetical: for part of this pass the bases
		// here had ALSO been cut by the same factors, and the game was running a 0.88 s slide, a
		// 0.09 s wall-jump window and a 0.77 retention while every individual number looked right.
		// -----------------------------------------------------------------------------------------
		UE_LOG(LogTraceGame, Display,
			TEXT("[SettingsDump:%s] SPECv9 move (base x scale = SHIPPED): gravity=%.3f | "
			     "airSoft %.0f x %.3f = %.0f, airHard %.0f x %.3f = %.0f | "
			     "slide %.3fs x %.3f = %.3fs | slideJump %.5f x %.3f (gainOnly=%d) = %.5f | "
			     "wallWindow %.3fs x %.3f = %.3fs, wallRetention %.3f x %.3f = %.3f"),
			Tag, Table.MovementGravityScale,
			Table.AirStrafeSoftCapSpeed, Table.AirStrafeAsymptoteScale,
			Table.AirStrafeSoftCapSpeed * Table.AirStrafeAsymptoteScale,
			Table.AirStrafeHardCapSpeed, Table.AirStrafeAsymptoteScale,
			Table.AirStrafeHardCapSpeed * Table.AirStrafeAsymptoteScale,
			Table.SlideDuration, Table.SlideMaxLengthScale,
			Table.SlideDuration * Table.SlideMaxLengthScale,
			Table.SlideJumpWindowSpeedBonus, Table.SlideJumpBonusScale,
			Table.bSlideJumpBonusScalesGainOnly ? 1 : 0,
			Table.bSlideJumpBonusScalesGainOnly
				? (1.f + (Table.SlideJumpWindowSpeedBonus - 1.f) * Table.SlideJumpBonusScale)
				: (Table.SlideJumpWindowSpeedBonus * Table.SlideJumpBonusScale),
			Table.WallJumpWindowSeconds, Table.WallJumpWindowScale,
			Table.WallJumpWindowSeconds * Table.WallJumpWindowScale,
			Table.WallJumpSpeedRetention, Table.WallJumpMomentumScale,
			Table.WallJumpSpeedRetention * Table.WallJumpMomentumScale);

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

		// -----------------------------------------------------------------------------------------
		// SPEC v13. The four features this pass tunes, on one line each, for the reason every block
		// above exists: these numbers are ALSO in Config/DefaultGame.ini and the ini wins.
		//
		// THE FIRST LINE IS A PARITY CHECK, not a listing. §3 is "update carrier speed to match the
		// new knife speed", so the two multipliers are one number written twice and the only failure
		// worth catching is them disagreeing — which is exactly what shipped for a pass after v12 §3
		// moved one of them. Printing the SPEEDS as well as the multipliers is deliberate: 1.22 and
		// 1.22 can still be different speeds if WalkSpeed is ever read from two places.
		// -----------------------------------------------------------------------------------------
		{
			const float CarrierSpeed = Table.WalkSpeed * Table.CarrierSpeedMultiplier;
			const float KnifeSpeed   = Table.WalkSpeed * Table.KnifeMoveSpeedMultiplier;
			const bool bParity = FMath::IsNearlyEqual(Table.CarrierSpeedMultiplier, Table.KnifeMoveSpeedMultiplier, 0.001f);
			UE_LOG(LogTraceGame, Display,
				TEXT("[SettingsDump:%s] SPECv13 §3 parity: carrier=%.4f (%.1f uu/s) knife=%.4f (%.1f uu/s) -> %s"),
				Tag, Table.CarrierSpeedMultiplier, CarrierSpeed,
				Table.KnifeMoveSpeedMultiplier, KnifeSpeed,
				bParity ? TEXT("MATCHED") : TEXT("*** BROKEN - v13 §3 asked for these to be equal ***"));
		}

		// §1's numbers are NOT on this page and are read across by /Script path — see the tombstone in
		// TraceSettings.h where they used to be declared. They shipped duplicated for one pass (this
		// page AND UTraceHealthSettings, both ini-backed, only the latter with a reader), so printing
		// them from the OWNING class is the thing that makes a re-divergence visible: if somebody
		// re-adds a copy here, this line still reports what the game is actually using.
		{
			float RegenDelay = -1.f;
			float RegenRate  = -1.f;
			bool  bRegenOn   = false;
			bool  bRegenFound = false;

			if (const UClass* HealthClass = FindObject<UClass>(nullptr, TEXT("/Script/Trace.TraceHealthSettings")))
			{
				if (const UObject* HealthCDO = HealthClass->GetDefaultObject())
				{
					const FFloatProperty* DelayProp = CastField<FFloatProperty>(HealthClass->FindPropertyByName(TEXT("RegenDelaySeconds")));
					const FFloatProperty* RateProp  = CastField<FFloatProperty>(HealthClass->FindPropertyByName(TEXT("RegenRatePerSecond")));
					const FBoolProperty*  OnProp    = CastField<FBoolProperty>(HealthClass->FindPropertyByName(TEXT("bRegenEnabled")));

					bRegenFound = (DelayProp != nullptr) && (RateProp != nullptr) && (OnProp != nullptr);
					if (bRegenFound)
					{
						RegenDelay = DelayProp->GetPropertyValue_InContainer(HealthCDO);
						RegenRate  = RateProp->GetPropertyValue_InContainer(HealthCDO);
						bRegenOn   = OnProp->GetPropertyValue_InContainer(HealthCDO);
					}
				}
			}

			const float DrawnReach = static_cast<float>(UTraceTrailComponent::GetTraceDrawnHalfReach());

			UE_LOG(LogTraceGame, Display,
				TEXT("[SettingsDump:%s] SPECv13 §1 regen [UTraceHealthSettings]: %s enabled=%d delay=%.2fs "
				     "rate=%.1fHP/s (0->%.0f in %.1fs) | "
				     "§5 magnet: radius=%.0fuu contestHysteresis=%.1fuu | "
				     "§8 landing: minDescent=%.1fdeg (surfaceMaxSlope=%.1fdeg) | "
				     "§7 trail: wallFit=%d clearanceByHalfWidth=%d margin=%.1f maxPush=%.1f (drawn reach %.1f + margin = %.1fuu asked for%s) maxInsert=%d"),
				Tag,
				bRegenFound ? TEXT("") : TEXT("*** PAGE NOT FOUND - regen numbers below are meaningless ***"),
				bRegenOn ? 1 : 0, RegenDelay, RegenRate,
				Table.MaxHealth,
				(RegenRate > 0.f) ? (Table.MaxHealth / RegenRate) : -1.f,
				Table.CoreCatchRadius, Table.CoreCatchContestHysteresisUU,
				Table.CoreLandingMinDescentDegrees, Table.CoreSurfaceMaxSlopeDegrees,
				Table.bTrailWallFitEnabled ? 1 : 0, Table.bTrailWallClearanceEnabled ? 1 : 0,
				Table.TrailWallFitMarginUU, Table.TrailWallFitMaxPushUU,
				// THE DRAWN REACH, NOT TrailRadius, and the difference is the whole of §7. This line
				// printed "halfWidth 22.5 + margin = 26.5uu asked for" for a pass, which is the exact
				// figure the fix exists to disprove: the ribbon overlaps its joints and the spline
				// overshoots corners, so it reaches ~36uu. Printing 26.5 beside a 44uu push made the
				// push look like generous headroom when it is in fact a 3.7uu margin. Read from
				// GetTraceDrawnHalfReach() rather than re-derived, so it cannot drift from the fitter.
				DrawnReach, DrawnReach + Table.TrailWallFitMarginUU,
				// And say so out loud if the setting is once again below what the fitter needs, because
				// the code silently floors it and this dump would otherwise be the last place that
				// looked fine.
				(Table.TrailWallFitMaxPushUU >= (DrawnReach + Table.TrailWallFitMarginUU))
					? TEXT("")
					: TEXT(" *** maxPush is BELOW this - the code's derived floor is overriding the setting ***"),
				Table.TrailWallFitMaxInsert);
		}

		// §6, with the arithmetic worked out rather than left to the reader. Power at a click and
		// Power at a full hold are the two ends of the linear rule, and the launch speeds beside them
		// are what those actually throw at once CoreMassScale has divided the impulse — which is the
		// number a playtest is judging.
		{
			const float Mass = FMath::Sqrt(FMath::Max(0.01f, Table.CoreMassScale));
			const float ClickPower = FMath::Clamp(Table.CoreThrowChargeFloorFraction, 0.f, 1.f);
			const float FullPower  = Table.bCoreThrowChargeClampsAtFull
				? 1.f
				: FMath::Max(1.f, Table.CoreThrowChargeMaxFraction);
			UE_LOG(LogTraceGame, Display,
				TEXT("[SettingsDump:%s] SPECv13 §6 charge: time=%.2fs floor=%.3f clampsAtFull=%d max=%.2f | "
				     "click -> %.3f x %.0f / sqrt(%.2f) = %.0f uu/s, full -> %.3f x %.0f / sqrt(%.2f) = %.0f uu/s "
				     "(+ thrower velocity x %.2f, added on top and NOT scaled by charge)"),
				Tag, Table.CoreThrowChargeSeconds, Table.CoreThrowChargeFloorFraction,
				Table.bCoreThrowChargeClampsAtFull ? 1 : 0, Table.CoreThrowChargeMaxFraction,
				ClickPower, Table.CoreThrowSpeed, Table.CoreMassScale, ClickPower * Table.CoreThrowSpeed / Mass,
				FullPower, Table.CoreThrowSpeed, Table.CoreMassScale, FullPower * Table.CoreThrowSpeed / Mass,
				Table.CoreThrowVelocityInheritance);
		}

		// -----------------------------------------------------------------------------------------
		// SPEC v14. Three lines, and each one exists because the number it prints CANNOT be trusted
		// from the header.
		//
		// LINE 1 IS THE MODE, AND IT IS THE MOST IMPORTANT LINE IN THIS DUMP THIS PASS. §2 is "change
		// game mode b to the default game mode", and the default lives in three layered places (this
		// header, DefaultGame.ini which outranks it, and a travel-URL override which outranks both).
		// The only honest answer is the CDO's value, which is what this prints. It also prints
		// whether characters are consequently ON, because §2 freezes mode A with no characters at
		// all — so "mode=A" and "charactersEnabled=True" together still means nobody has a character,
		// and reading only the toggle would be reading the wrong half.
		//
		// LINE 2 IS THE TWO DERIVED NUMBERS §6 explicitly says to derive rather than hardcode. Mace's
		// magnet is a fraction of CoreCatchRadius and her spike pull is a multiple of the air-strafe
		// hard cap; both PRODUCTS are printed beside their bases, because the product is the number
		// the game is played at and it appears nowhere else. 585 on this line is correct; 450 or
		// 760.5 would be the two bugs (base not applied, or applied twice).
		//
		// LINE 3 IS THE COOLDOWN LADDER, because §6 gives 20s four times and 25s once, and "X is on
		// 20 like everyone else" is exactly the kind of thing that ships unnoticed.
		{
			const float MaceMagnetRadius = Table.CoreCatchRadius * (1.f + Table.MaceMagnetRadiusBonus);
			const float MomentumCeiling  = Table.AirStrafeHardCapSpeed * Table.AirStrafeAsymptoteScale;

			UE_LOG(LogTraceGame, Display,
				TEXT("[SettingsDump:%s] SPECv14 §2 mode: ScoringMode=%s (v14 wants B) | §3 charactersEnabled=%d "
				     "(and mode A forces everyone characterless regardless) | §4 carrierControlImmune=%d [ASSUMPTION]"),
				Tag,
				(Table.ScoringMode == ETraceScoringMode::ThrownCoreAndGoals) ? TEXT("B-ThrownCoreAndGoals") : TEXT("A-EndzoneStatusCore"),
				Table.bCharactersEnabled ? 1 : 0,
				Table.bCarrierImmuneToAbilityControl ? 1 : 0);

			UE_LOG(LogTraceGame, Display,
				TEXT("[SettingsDump:%s] SPECv14 §6 derived (base x bonus = PLAYED): Mace magnet %.0f x (1 + %.3f) = %.0fuu "
				     "(§6 says 585) | Mace spike pull = airHardCap %.0f x %.3f x %.3f = %.0f uu/s | "
				     "Rocco ripple dash = DashSpeed %.0f x %.3f = %.0f uu/s for %.3fs x %.3f"),
				Tag, Table.CoreCatchRadius, Table.MaceMagnetRadiusBonus, MaceMagnetRadius,
				Table.AirStrafeHardCapSpeed, Table.AirStrafeAsymptoteScale, Table.MaceSpikePullSpeedMultiplier,
				MomentumCeiling * Table.MaceSpikePullSpeedMultiplier,
				Table.DashSpeed, Table.RoccoRippleDashSpeedMultiplier, Table.DashSpeed * Table.RoccoRippleDashSpeedMultiplier,
				Table.DashDuration, Table.RoccoRippleDashDurationMultiplier);

			UE_LOG(LogTraceGame, Display,
				TEXT("[SettingsDump:%s] SPECv14 §6 cooldowns (s): default=%.1f | Rocco ripple=%.1f Chut chud=%.1f "
				     "Mace spike=%.1f [ASSUMPTION] Oyster pickler=%.1f [ASSUMPTION] X sting=%.1f (the only 25) | "
				     "Rocco stack %.0f%% x%d for %.2fs | Chud -%.0f%% for %.1fs | poison %.0f/%.2fs for %.1fs, slow %.0f%%"),
				Tag, Table.AbilityDefaultCooldownSeconds,
				Table.RoccoRippleCooldownSeconds, Table.ChudCooldownSeconds, Table.MaceSpikeCooldownSeconds,
				Table.OysterPicklerCooldownSeconds, Table.XStingCooldownSeconds,
				Table.RoccoHeadshotSpeedBonusPerStack * 100.f, Table.RoccoHeadshotSpeedStackMax,
				Table.RoccoHeadshotSpeedDurationSeconds,
				Table.ChudDamageReduction * 100.f, Table.ChudDurationSeconds,
				Table.OysterPoisonDamagePerTick, Table.OysterPoisonTickIntervalSeconds,
				Table.OysterPoisonDurationSeconds, Table.OysterPoisonSlowFraction * 100.f);
		}

		// -----------------------------------------------------------------------------------------
		// SPEC v13 §4 — WHICH LEVEL DOES THE GAME OPEN. Verbatim: "The game should default to opening
		// the main menu level."
		//
		// THIS IS HERE BECAUSE THE ANSWER CANNOT BE READ FROM Config/DefaultEngine.ini EITHER. The
		// three map keys are layered exactly like every gameplay number on this page — Base, then
		// Default, then any platform or user override — so the only honest answer is what
		// UGameMapsSettings holds at runtime, which is what the engine itself reads:
		//   * a standalone/packaged launch with no map on the command line opens GameDefaultMap
		//     (UEngine::Browse -> UGameMapsSettings::GetGameDefaultMap);
		//   * the EDITOR opens EditorStartupMap at startup (FEditorFileUtils::LoadDefaultMapAtStartup),
		//     and Play-in-Editor then plays whatever level is open — which is why an EditorStartupMap
		//     pointing at the arena means Play skips the menu, the actual v13 §4 symptom;
		//   * a dedicated server opens ServerDefaultMap, which stays on the arena: a server has
		//     nobody to show a menu to.
		//
		// Resolved BY CLASS PATH rather than by including GameMapsSettings.h, for the same reason
		// FKnobSpec::OwnerPath is: UGameMapsSettings lives in the EngineSettings module, this module
		// does not link it, and a diagnostic must never be the thing that adds a build dependency.
		// EditorStartupMap is WITH_EDITORONLY_DATA, so it is absent in a true no-editor build and the
		// line says so rather than printing a lie.
		// -----------------------------------------------------------------------------------------
		if (const UClass* MapsClass = FindObject<UClass>(nullptr, TEXT("/Script/EngineSettings.GameMapsSettings")))
		{
			const UObject* MapsCDO = MapsClass->GetDefaultObject();

			auto ReadMapKey = [MapsClass, MapsCDO](const TCHAR* PropertyName) -> FString
			{
				const FProperty* Found = MapsClass->FindPropertyByName(FName(PropertyName));
				if (Found == nullptr || MapsCDO == nullptr)
				{
					return TEXT("<not in this build>");
				}
				FString Value;
				Found->ExportText_InContainer(0, Value, MapsCDO, MapsCDO, nullptr, PPF_None);
				return Value.IsEmpty() ? TEXT("<empty>") : Value;
			};

			const FString GameDefault   = ReadMapKey(TEXT("GameDefaultMap"));
			const FString EditorStartup = ReadMapKey(TEXT("EditorStartupMap"));
			const FString ServerDefault = ReadMapKey(TEXT("ServerDefaultMap"));

			UE_LOG(LogTraceGame, Display,
				TEXT("[SettingsDump:%s] SPECv13 §4 maps: GameDefaultMap=%s | EditorStartupMap=%s | ServerDefaultMap=%s"),
				Tag, *GameDefault, *EditorStartup, *ServerDefault);

			const bool bGameMenu   = GameDefault.Contains(TEXT("MainMenu"));
			const bool bEditorMenu = EditorStartup.Contains(TEXT("MainMenu"));
			UE_LOG(LogTraceGame, Display,
				TEXT("[SettingsDump:%s] SPECv13 §4 verdict: standalone launch -> %s | editor startup + Play -> %s"),
				Tag,
				bGameMenu ? TEXT("MENU (correct)") : TEXT("*** NOT the menu ***"),
				bEditorMenu ? TEXT("MENU (correct)") : TEXT("*** NOT the menu - Play would skip the menu ***"));
		}

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

		if (MutableTable == nullptr || Edited == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[LiveEdit] '%s' is not a property of UTraceSettings."), *PropertyName);
			return;
		}

		// FLOATS, BOOLS AND INTS, not floats alone. This used to reject anything that was not an
		// FFloatProperty, which quietly meant every switch on the page — bAirStrafeGainFalloff,
		// bWallJumpEnabled, bSlideJumpBonusScalesGainOnly, bDeferPeriodEndToPlayBreak — was NOT
		// live-editable and could only be moved by an ini edit and a restart. A knob that needs a
		// restart is half a knob, and the bool ones are exactly the knobs a playtest wants to flip
		// mid-session to compare two readings of a note.
		//
		// Non-zero is true, matching every other console-driven bool in the engine.
		float PreviousValue = 0.f;
		if (FFloatProperty* EditedFloat = CastField<FFloatProperty>(Edited))
		{
			PreviousValue = EditedFloat->GetPropertyValue_InContainer(MutableTable);
			EditedFloat->SetPropertyValue_InContainer(MutableTable, NewValue);
		}
		else if (FBoolProperty* EditedBool = CastField<FBoolProperty>(Edited))
		{
			PreviousValue = EditedBool->GetPropertyValue_InContainer(MutableTable) ? 1.f : 0.f;
			EditedBool->SetPropertyValue_InContainer(MutableTable, !FMath::IsNearlyZero(NewValue));
		}
		else if (FIntProperty* EditedInt = CastField<FIntProperty>(Edited))
		{
			PreviousValue = static_cast<float>(EditedInt->GetPropertyValue_InContainer(MutableTable));
			EditedInt->SetPropertyValue_InContainer(MutableTable, FMath::RoundToInt(NewValue));
		}
		else
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[LiveEdit] '%s' exists but is a %s — this path edits float, bool and int knobs only."),
				*PropertyName, *Edited->GetClass()->GetName());
			return;
		}

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

	// Enum was added in v9 for BotDifficulty (spec §9). A UPROPERTY of an `enum class X : uint8`
	// becomes an FEnumProperty in UE5, NOT an FByteProperty — both are accepted below because a
	// UENUM declared without an explicit underlying type still lands as the latter, and a knob that
	// reported DEAD purely because of which of the two UHT chose would be a false alarm in the one
	// tool whose job is to have no false alarms.
	// Struct was added by spec v14: Rocco's two ripple-ring COLOURS are FLinearColor properties, and
	// a colour a designer can move is a knob like any other. Without this case they would have had
	// to be left out of the table — i.e. the two knobs most likely to be typo'd in an ini
	// (Color/Colour, and a struct literal the parser can silently reject) would have been the two
	// nothing checked.
	enum class EKnobType : uint8 { Float, Bool, Int, Enum, Struct };

	struct FKnobSpec
	{
		const TCHAR* Name;
		EKnobType Type;
		const TCHAR* Note;

		/**
		 * Which settings class owns this knob, as a /Script path. Null means UTraceSettings.
		 *
		 * SPEC v10 ADDED THIS because v10 is the first pass whose knobs do not all live on this page.
		 * The knife's weapon numbers are a separate UDeveloperSettings (UTraceMeleeSettings), which is
		 * the right home for them — but it shipped with header literals and NO ini section, i.e. the
		 * exact "slider that silently does nothing" failure this whole command exists to catch, just
		 * one class over. A verifier that can only see one class would have called that a clean run.
		 *
		 * Resolved by PATH at runtime rather than by #including the other header, deliberately: this
		 * file must keep compiling while another module's settings class is being edited, and a knob
		 * table that can break the build is a knob table people delete.
		 */
		const TCHAR* OwnerPath = nullptr;
	};

	void VerifyTraceKnobs()
	{
		static const FKnobSpec Knobs[] =
		{
			// --- spec v5 §5, fire rate ---------------------------------------------------------
			// The DESCRIPTION deliberately no longer restates the number: Trace.VerifyKnobs prints
			// this string immediately beside the LIVE value, so the old "150 RPM = 0.40s" was a
			// contradiction on screen the moment spec v24 §4 moved the gun to 190 RPM (0.3158 s).
			{ TEXT("FireInterval"),                    EKnobType::Float, TEXT("seconds between rounds; RPM = 60 / this. Per-character abilities SCALE it via GetFireIntervalScaleFor()") },

			// --- spec v16 §1, ammo (a system that did not exist before that pass) ---------------
			// There is deliberately NO reserve/carried-ammo row: the reserve is infinite and has no
			// knob, so a row here would be the kind of dead entry this table exists to expose.
			{ TEXT("ClipSize"),                        EKnobType::Int,   TEXT("v16 §1: '30 bullets per clip'. Reserve is INFINITE and has no knob") },
			{ TEXT("ReloadSeconds"),                   EKnobType::Float, TEXT("v16 §1: 'Reloading takes .5seconds'; a shared-clock deadline, not a countdown") },

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

			// --- spec v5 §7, the ledge block. THE MANTLE'S EIGHT KNOBS ARE GONE (spec v12 §5) ---
			//
			// bMantleEnabled, MantleReachUU, MantleMinHeightUU, MantleMaxHeightUU,
			// MantleDurationSeconds, MantleUpPhaseFraction, MantleCooldownSeconds and
			// MantleMinForwardSpeed were removed from UTraceSettings when the ability was removed, and
			// their rows are deleted here rather than left to report DEAD: this table's job is to catch
			// a knob whose READER lost it, and these are the opposite case — the reader was deleted on
			// purpose and the knob went with it. A permanently-red row trains people to ignore the
			// summary line, which is the one thing this command cannot afford.
			//
			// LedgeGroundGraceSeconds STAYS and is now the whole section. It was never a mantle knob;
			// it is the prediction-desync half of the original Demo 5 complaint, and with the mantle
			// gone it is the only thing left addressing "no bug when a player hits the top edge of an
			// obstacle" (spec v12 §5). If this one ever reads DEAD the ledge bug comes straight back.
			{ TEXT("LedgeGroundGraceSeconds"),         EKnobType::Float, TEXT("v12 §5: the ledge desync fix, ALL that remains of the mantle block [by-name bind]") },

			// --- spec v5 §4, mode B geometry and weight ----------------------------------------
			{ TEXT("GoalWidthFieldFraction"),          EKnobType::Float, TEXT("2000uu goal mouth") },
			{ TEXT("GoalHeightUU"),                    EKnobType::Float, TEXT("goal APPROACH RAMP height since v6 4.3") },

			// --- spec v6 §4.1, the mode-B catch zone  [ALL THREE BOUND BY NAME by ATraceCore] ---
			{ TEXT("CoreCatchRadius"),                 EKnobType::Float, TEXT("v12 §4: 500 -> 450, MUST read 450 [by-name bind]") },
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

			// --- spec v9, every knob this pass introduced or moved -----------------------------
			//
			// §9 the default bot difficulty. It is on this list even though nothing binds it by name,
			// because the failure mode is the same shape: the value the game uses comes from
			// DefaultGame.ini, the header literal is only what a reader sees, and "I changed the
			// default and it still starts on Easy" is exactly what a missing ini key looks like.
			// MUST PRINT Normal.
			{ TEXT("BotDifficulty"),                   EKnobType::Enum,  TEXT("v9 §9: Easy -> Normal, must read Normal") },

			// §11 the deferred whistle. Both are read AT THE POINT OF USE by ATraceGameMode, so they
			// retune live; the cap is the stalemate guard rail and must never ship at 0.
			{ TEXT("bDeferPeriodEndToPlayBreak"),      EKnobType::Bool,  TEXT("v9 §11: clock expiry ARMS the whistle, next dead ball blows it") },
			{ TEXT("PeriodEndMaxDeferSeconds"),        EKnobType::Float, TEXT("v9 §11: hard cap on the defer, 60s, 0 = never force it") },

			// §§5-8 THE MOVEMENT SCALARS. Every one of these is a v9 change expressed as its own
			// named multiplier over the designer's existing base value, rather than as an edit to the
			// base — so re-tuning the base and re-tuning the v9 change never fight, and reverting a
			// v9 number is one line rather than an archaeology exercise.
			//
			// ALL EIGHT ARE BOUND BY NAME by UTraceCharacterMovementComponent through TraceMoveKnob,
			// and every one of them shipped DEAD for part of this pass: the component asked for
			// "MovementGravityScale" while this page declared "PlayerGravityScale", and the other
			// seven had no property here at all. The component's own MOVEKNOB report said FALLBACK
			// for all five it had reached, the build was green, the settings panel looked complete,
			// and the game ran at 1.0 gravity. THAT is the failure this table exists to catch.
			//
			// The base + scale pairs, and what the product must come to:
			//   MovementGravityScale     1.12                                    (§8, gravity x1.12)
			//   AirStrafeAsymptoteScale  950 x 1.10 = 1045, 1250 x 1.10 = 1375   (§8, asymptote +10%)
			//   SlideMaxLengthScale      1.80 x 0.70 = 1.26 s                    (§6, length -30%)
			//   SlideJumpBonusScale      1 + 0.3125 x 1.30 = 1.40625             (§7, bonus +30%)
			//   WallJumpWindowScale      0.25 x 0.60 = 0.15 s                    (§5, shorter window)
			//   WallJumpMomentumScale    0.95 x 0.90 = 0.855                     (§5, -10% momentum)
			// The bases themselves are already listed in the v5/v8 blocks above and are deliberately
			// NOT repeated here — a second row would double-count them in the bound/dead summary —
			// but their printed values there are half of the evidence: base x scale is the shipped
			// number, and a base that has ALSO been cut is the double-application bug.
			{ TEXT("MovementGravityScale"),            EKnobType::Float, TEXT("v9 §8: x1.12, less floaty [by-name bind]") },
			{ TEXT("AirStrafeAsymptoteScale"),         EKnobType::Float, TEXT("v9 §8: x1.10 over BOTH air caps -> 1045/1375 [by-name bind]") },
			{ TEXT("SlideMaxLengthScale"),             EKnobType::Float, TEXT("v9 §6: x0.70 over SlideDuration -> 1.26s [by-name bind]") },
			{ TEXT("SlideJumpBonusScale"),             EKnobType::Float, TEXT("v9 §7: x1.30 over the bonus -> 1.40625 [by-name bind]") },
			{ TEXT("bSlideJumpBonusScalesGainOnly"),   EKnobType::Bool,  TEXT("v9 §7: true = scale the gain (1.40625), false = the whole multiplier (1.70625) [by-name bind]") },
			{ TEXT("WallJumpWindowScale"),             EKnobType::Float, TEXT("v9 §5: x0.60 over the contact window -> 0.15s [by-name bind]") },
			{ TEXT("WallJumpMomentumScale"),           EKnobType::Float, TEXT("v9 §5: x0.90 over retention -> 0.855 [by-name bind]") },
			// WallJumpMantleLockoutSeconds (v9 §5) is deleted with the mantle — see the v12 §5 note in
			// the ledge block above. Its only job was to stop the mantle undoing a wall jump.

			// --- spec v10, every knob this pass introduced or moved ----------------------------
			//
			// §5 WALL JUMPING, THE THIRD COMPLAINT. All three are resolved by TraceMoveKnob, i.e. BY
			// NAME, so a typo reverts the whole v10 wall-jump fix to the component's own literals
			// while leaving a green build and a full-looking settings panel. Two of them are not
			// re-tunings of anything - they are the mechanism v10 added after v9's second attempt at
			// shaving the input window failed to change the feel - so they are listed on the pass
			// that introduces them, per the v8 note above.
			{ TEXT("WallJumpControlLockoutSeconds"),   EKnobType::Float, TEXT("v10 §5: THE stickiness fix - held input cannot steer back into the wall [by-name bind]") },
			{ TEXT("WallJumpInputBufferSeconds"),      EKnobType::Float, TEXT("v10 §5: an early jump press is spent on the first legal frame [by-name bind]") },
			{ TEXT("WallJumpMomentumScaleV10"),        EKnobType::Float, TEXT("v10 §5: the further x0.90 -> shipped retention 0.95 x 0.90 x 0.90 = 0.7695 [by-name bind]") },

			// §1 THE KNIFE'S MOVEMENT KIT. The weapon's own numbers are on UTraceMeleeSettings below;
			// these three multiply WalkSpeed and the two air caps, which are on this page, and are
			// read through TraceMoveKnob with the rest of the movement scalars.
			// v12 §3 MOVED ALL THREE. "Reduce max speed with the knife from the previous 30% increase
			// to 22% and adjust momentum accordingly." The ground multiplier is the request; the two
			// air ceilings are the "adjust momentum accordingly", scaled by BONUS (the part above 1)
			// rather than by the multiplier itself, which would have pushed the knife below the gun.
			//
			// *** READ THE CARRIER LINE BELOW THESE THREE. v12 §3 left CarrierSpeedMultiplier at 1.30
			// *** while the knife went to 1.22, which made the CARRIER the faster of the two for one
			// *** pass. SPEC v13 §3 CLOSES IT — "update carrier speed to match the new knife speed" —
			// *** so both now read 1.22. The two rows are printed together on purpose: this pair is
			// *** the parity, and the only way to see it broken is to see both numbers at once.
			{ TEXT("KnifeMoveSpeedMultiplier"),        EKnobType::Float, TEXT("v12 §3: 1.30 -> 1.22, MUST read 1.22 [by-name bind]") },
			{ TEXT("KnifeAirStrafeSoftCapMultiplier"), EKnobType::Float, TEXT("v12 §3: 1+0.25x22/30 = 1.1833, MUST read 1.1833 [by-name bind]") },
			{ TEXT("KnifeAirStrafeHardCapMultiplier"), EKnobType::Float, TEXT("v12 §3: 1+0.35x22/30 = 1.2567, MUST stay >= the soft multiplier [by-name bind]") },
			// The knife's counterpart. Not moved by v12 and printed here so the broken parity is on the
			// same screen as the number that broke it, rather than being rediscovered in a playtest.
			{ TEXT("CarrierSpeedMultiplier"),          EKnobType::Float, TEXT("v13 §3: 1.30 -> 1.22, MUST equal KnifeMoveSpeedMultiplier above - parity restored") },

			// §4 the parry. Not new, but it MOVED this pass (0.20 -> 0.175) and it is the one number
			// the whole mechanic is - window and invulnerability are the SAME property, which is the
			// question v10 §4 asked - so it is worth a line that proves the ini still drives it. It
			// is already listed in the v8 block above and is deliberately NOT repeated here.

			// §3 the turnover grace. Also not new, but v10 §3 is a REPEAT complaint ("the grace
			// period on turnovers doesn't seem to be working"), and the first thing to rule out when
			// a mechanic "doesn't seem to be working" is that its knob reaches the code at all. It
			// has never been on this list. It is now.
			{ TEXT("CoreTurnoverGraceSeconds"),        EKnobType::Float, TEXT("v10 §3: seconds the new holder's trace does not FORM after a turnover between teams") },

			// --- spec v12, every knob this pass introduced or moved ----------------------------
			//
			// §6 THE TRACE CLIPPING INTO WALLS. All four are new this pass and all four are read by
			// UTraceTrailComponent, so they are listed on the pass that introduces them per the v8
			// rule. Each pairs with the Trace.Trail.WallFit* console variable of the same meaning and
			// must resolve the way GetTraceTrailRadius() resolves TrailRadius — settings property by
			// default, CVar when the CVar is explicitly set. Until the component reads them these
			// rows say OK (the properties exist) while the panel still moves nothing, which is the
			// ONE case this table cannot see: it proves a knob is reachable, not that anybody reached
			// for it. §6's own verification is what closes that half.
			//
			// The invariant they exist under is the one this project has already broken in both
			// directions: the fitter edits TrailPoints, which the trip test and every renderer arm
			// both read, so lethal == drawn survives the fix by construction. Anything that edited
			// the RIBBON instead would build an invisible kill volume and this table would still say
			// OK.
			{ TEXT("bTrailWallFitEnabled"),            EKnobType::Bool,  TEXT("v12 §6: corner fitter on/off - OFF reproduces the reported bug for A/B") },
			{ TEXT("TrailWallFitMarginUU"),            EKnobType::Float, TEXT("v12 §6: clearance over the trace's own half width; too large = nothing routes") },
			{ TEXT("TrailWallFitMaxPushUU"),           EKnobType::Float, TEXT("v12 §6: cap on nudging an already-embedded point; residue cleanup only") },
			{ TEXT("TrailWallFitMaxInsert"),           EKnobType::Int,   TEXT("v12 §6: per-append insert budget; exhausting it falls back to the chord") },

			// §4 and §3 are recorded on the rows they moved (CoreCatchRadius in the v6 block,
			// the three Knife* rows in the v10 block) rather than repeated here — a second row would
			// double-count them in the bound/dead summary, which is the same rule v8 §8 set.
			//
			// §5's removals are recorded as deletions in the ledge block above.

			// §7 the half length is NOT here, and that is not an oversight: HalfDuration is a
			// UPROPERTY(config) on ATraceGameMode, not on this page, so this command cannot see it.
			// Trace.DumpSettings prints it from the running game mode, which is where it is read.

			// --- spec v13, every knob this pass introduced or moved ----------------------------
			//
			// ALL EIGHT ARE NEW OR MOVED THIS PASS and every one of them is listed on the pass that
			// introduces it, per the v8 rule, EVEN THOUGH THREE OF THE FEATURES ARE STILL BEING
			// WRITTEN in other files as this lands. That is the point of the rule: this table proves
			// a knob EXISTS, is `config` (so DefaultGame.ini reaches it) and is EditAnywhere (so the
			// panel does). It cannot prove anybody reads it — see the v12 §6 note above, which says
			// the same thing about the wall-fit block and was right to. The readers' own binding logs
			// close that half, and the pass report names the exact property each slice must read.
			//
			// §1 HEALTH REGENERATION. "After 9 seconds without taking damage, characters' health
			// slowly begins to regenerate." The delay is the user's number; the rate is the
			// [ASSUMPTION]. Read at the point of use by the health path so a live edit retunes a
			// match in progress rather than the next one.
			// §1 REGENERATION IS ON UTraceHealthSettings, and the two rows that were here
			// (HealthRegenDelaySeconds / HealthRegenPerSecond, on UTraceSettings) ARE DELETED WITH THE
			// PROPERTIES THEY NAMED. Both pages shipped for one pass, both ini-backed, both spelled
			// correctly — and only one had a reader. These rows reported OK for the copy nothing read,
			// which is the one blind spot this table has by construction: it proves a property is
			// reachable, never that anybody reached for it. The rows below name the surviving page by
			// /Script path, so they verify the knobs the game actually resolves.
			{ TEXT("bRegenEnabled"),      EKnobType::Bool,  TEXT("v13 §1: master switch; 0 is the RegenTest RED arm"), TEXT("/Script/Trace.TraceHealthSettings") },
			{ TEXT("RegenDelaySeconds"),  EKnobType::Float, TEXT("v13 §1: 9s untouched before regen starts - the user's number, MUST read 9"), TEXT("/Script/Trace.TraceHealthSettings") },
			{ TEXT("RegenRatePerSecond"), EKnobType::Float, TEXT("v13 §1: [ASSUMPTION] 10 HP/s = 0->100 in 10s. Keep well under the gun's 100 HP/s"), TEXT("/Script/Trace.TraceHealthSettings") },

			// §5 THE CONTESTED MAGNET. The nearest-player rule needs no knob; the TIE does, or two
			// chasers at similar range swap the Core between them several times a second. 0 is the
			// raw rule with no hysteresis, which is the arm that reproduces the flicker.
			{ TEXT("CoreCatchContestHysteresisUU"),    EKnobType::Float, TEXT("v13 §5: a rival must be closer by MORE than this to steal the magnet. 0 = raw nearest-wins") },

			// §6 THE CHARGE-UP THROW. All four numbers the spec asked for by name: charge time,
			// floor fraction, whether it clamps, and the ceiling when it does not. Charge scales the
			// IMPULSE; CoreThrowVelocityInheritance is still added on top and is listed in the v8
			// block above rather than repeated here.
			{ TEXT("CoreThrowChargeSeconds"),          EKnobType::Float, TEXT("v18: 0.6s hold = full momentum (1.0 in v13, 0.8 in v16); the crosshair ring is read against this") },
			{ TEXT("CoreThrowChargeFloorFraction"),    EKnobType::Float, TEXT("v13 §6: [ASSUMPTION] 0.15 - an instant click is 'very low', never zero") },
			{ TEXT("bCoreThrowChargeClampsAtFull"),    EKnobType::Bool,  TEXT("v13 §6: [ASSUMPTION] true - holding past the charge time adds nothing") },
			{ TEXT("CoreThrowChargeMaxFraction"),      EKnobType::Float, TEXT("v13 §6: the ceiling when the clamp is OFF. 1.0 so unticking the box changes nothing by itself") },

			// §7 THE TRACE STILL CLIPS INTO WALLS. TrailWallFitMaxPushUU is NOT a new row - it is in
			// the v12 §6 block above and its note there records the 12 -> 30 move and the measurement
			// that forced it (26.1 uu of penetration against a 12 uu allowance). Only the new switch
			// is listed here, and it pairs with Trace.Trail.WallClearance the way the other four pair
			// with their CVars.
			{ TEXT("bTrailWallClearanceEnabled"),      EKnobType::Bool,  TEXT("v13 §7: clear by the trace's own half width near geometry, not only on a blocked chord. OFF = v12 behaviour. NOW READ by WallClearanceEnabled() - it was declared with NO reader for a pass") },

			// §8 THE MID-AIR TURNOVER. Added during integration: TraceCore's own startup binding check
			// listed this name as the one v13 knob with no property behind it, so Trace.ModeB.Landing-
			// MinDescentDegrees was the only way to retune the fix. It has a property now, and this row
			// is what keeps the two spellings tied together.
			{ TEXT("CoreLandingMinDescentDegrees"),    EKnobType::Float, TEXT("v13 §8: arrival angle for a contact to be a LANDING; 0 restores the pre-v13 'any upward normal' bug for A/B") },

			// --- spec v10 §1, THE KNIFE ITSELF - a DIFFERENT settings class ---------------------
			//
			// UTraceMeleeSettings, config = Game, section [/Script/Trace.TraceMeleeSettings]. Every
			// one of these was a header literal with no ini key behind it until v10 reconciled them.
			{ TEXT("BackstabDamage"),            EKnobType::Float, TEXT("v10 §1: 100 from behind"),                    TEXT("/Script/Trace.TraceMeleeSettings") },
			{ TEXT("FrontDamage"),               EKnobType::Float, TEXT("v10 §1: 30 from the front"),                  TEXT("/Script/Trace.TraceMeleeSettings") },
			// v12 §1 MOVES THIS ONE: 90 -> 30. Verbatim: "Change the 'back' zone for knife damage
			// being 100 from a boundary at 90 degrees to 60 degrees, with the center of the 60 angle
			// being at the center of the model."
			//
			// SIXTY IS THE CONE, THIRTY IS THIS PROPERTY, and getting that backwards is the whole
			// risk in the change. The property is a HALF-angle measured from directly behind the
			// victim, so a 60 degree cone centred on the rear axis is +/-30. Entering 60 here would
			// ship a 120 degree back zone — a WIDENING of the instant-kill arc dressed up as the
			// narrowing that was asked for, and every automated check would pass. The row prints the
			// value so an azimuth sweep and this table can be compared directly.
			{ TEXT("BackstabHalfAngleDegrees"),  EKnobType::Float, TEXT("v12 §1: 90 -> 30, i.e. a 60deg CONE (+/-30). MUST read 30, NOT 60"), TEXT("/Script/Trace.TraceMeleeSettings") },
			{ TEXT("SwingCooldownSeconds"),      EKnobType::Float, TEXT("v10 §1: 0.5s between swings"),                TEXT("/Script/Trace.TraceMeleeSettings") },
			{ TEXT("SwapSeconds"),               EKnobType::Float, TEXT("v10 §1: 0.2s pullout, BOTH directions"),      TEXT("/Script/Trace.TraceMeleeSettings") },
			// v12 §2 turns the swipe into a stab, so these two are now THE STAB'S TIMING: the windup
			// is the lead-in before the blade goes live and the anim is thrust-plus-return. The names
			// are deliberately NOT changed to Stab* — they are ini keys with values behind them in
			// DefaultGame.ini, and renaming a live key is how a tuned value silently reverts to a
			// header literal. What they drive changed; what they are called did not.
			{ TEXT("SwingWindupSeconds"),        EKnobType::Float, TEXT("v12 §2: lead-in before the STAB goes live"),  TEXT("/Script/Trace.TraceMeleeSettings") },
			{ TEXT("SwingAnimSeconds"),          EKnobType::Float, TEXT("v12 §2: thrust + return, must stay under the cooldown"), TEXT("/Script/Trace.TraceMeleeSettings") },
			{ TEXT("SwingRangeUU"),              EKnobType::Float, TEXT("v10 §1: blade reach"),                        TEXT("/Script/Trace.TraceMeleeSettings") },
			{ TEXT("SwingArcDegrees"),           EKnobType::Float, TEXT("v10 §1: a sweep, not a hitscan point"),       TEXT("/Script/Trace.TraceMeleeSettings") },
			{ TEXT("SwingSamples"),              EKnobType::Int,   TEXT("v10 §1: rays fanned across the arc"),         TEXT("/Script/Trace.TraceMeleeSettings") },
			// KnifeSpeedMultiplier and KnifeAirCapScale are DELIBERATELY ABSENT from this block. They
			// existed on UTraceMeleeSettings for part of this pass and were removed once the knife's
			// mobility landed where it belongs - beside the WalkSpeed and air caps it multiplies, on
			// the UTraceSettings page above. Two knobs for one number is how a base and its scalar end
			// up fighting, and this comment is here so nobody re-adds them out of symmetry.
			{ TEXT("BotEngageRangeUU"),          EKnobType::Float, TEXT("v10 §1: bots must use it to playtest it"),    TEXT("/Script/Trace.TraceMeleeSettings") },
			{ TEXT("BotDisengageRangeUU"),       EKnobType::Float, TEXT("v10 §1: > engage, or a bot swaps every frame"), TEXT("/Script/Trace.TraceMeleeSettings") },
			{ TEXT("BotSwingRangeFraction"),     EKnobType::Float, TEXT("v10 §1: swing inside its reach, not at the edge"), TEXT("/Script/Trace.TraceMeleeSettings") },

			// =========================================================================================
			// SPEC v14 §§2, 4, 5, 6 — THE ABILITY FRAMEWORK AND ALL FIVE CHARACTERS
			//
			// Every ability constant from every character in §6 is a row here, and that is a
			// requirement of the pass rather than diligence: five character agents are about to read
			// these values BY NAME out of UTraceSettings, and a misspelled property is a silent no-op
			// that their code cannot detect. This table is the only thing that turns that into a
			// visible failure before anybody plays a match.
			//
			// The ONE knob on this list that touches the carrier invariant is
			// bCarrierImmuneToAbilityControl, and it is the §4 [ASSUMPTION] about slows/pulls only.
			// There is deliberately NO knob for "abilities may damage carriers".
			// =========================================================================================

			// --- v14 §2: which mode a fresh install plays -------------------------------------------
			{ TEXT("ScoringMode"),                     EKnobType::Enum,  TEXT("v14 §2: MUST read ThrownCoreAndGoals — mode B is the default now") },

			// --- v14 §§3-5: the framework ------------------------------------------------------------
			{ TEXT("bCharactersEnabled"),              EKnobType::Bool,  TEXT("v14 §3: the 'turn off all characters' toggle") },
			{ TEXT("bCarrierImmuneToAbilityControl"),  EKnobType::Bool,  TEXT("v14 §4 ASSUMPTION: carriers also immune to slows/pulls/knockbacks. Damage immunity has NO knob") },
			{ TEXT("AbilityDefaultCooldownSeconds"),   EKnobType::Float, TEXT("v14 §5: 20s, the cooldown a character gets if it names none") },
			{ TEXT("CharacterSelectTimeoutSeconds"),   EKnobType::Float, TEXT("v14 §3 ASSUMPTION: auto-assign so one idle player cannot stall the match") },

			// --- v14 §6: Rocco ------------------------------------------------------------------------
			{ TEXT("RoccoHeadshotSpeedBonusPerStack"), EKnobType::Float, TEXT("v14 §6: 3% per headshot kill") },
			{ TEXT("RoccoHeadshotSpeedStackMax"),      EKnobType::Int,   TEXT("v14 §6 ASSUMPTION: cap the stack (10 = +30%)") },
			{ TEXT("RoccoHeadshotSpeedDurationSeconds"), EKnobType::Float, TEXT("v24 §11: ONE 3s timer (was 1s) for the whole stack, refreshed per kill") },
			{ TEXT("RoccoSecondJumpZVelocity"),        EKnobType::Float, TEXT("v14 §6: 'a very small second jump'") },
			{ TEXT("RoccoSecondJumpRedirectFraction"), EKnobType::Float, TEXT("v14 §6: change direction midair, INSTANTLY (1 = instant)") },
			{ TEXT("RoccoRippleLifetimeSeconds"),      EKnobType::Float, TEXT("v14 §6: 4s, then all effects and visuals vanish") },
			{ TEXT("RoccoRippleCooldownSeconds"),      EKnobType::Float, TEXT("v14 §6: 20s, SEPARATE from the standard dash") },
			{ TEXT("RoccoRippleDashSpeedMultiplier"),  EKnobType::Float, TEXT("v14 §6: derived from Movement|Dash DashSpeed") },
			{ TEXT("RoccoRippleDashDurationMultiplier"), EKnobType::Float, TEXT("v14 §6: derived from DashDuration; sets the path length") },
			{ TEXT("RoccoRippleRideSpeedMultiplier"),  EKnobType::Float, TEXT("v14 §6: riders are propelled along the path and MAY SHOOT") },
			{ TEXT("RoccoRippleEntryRadiusUU"),        EKnobType::Float, TEXT("v14 §6: entry is at the START ring only") },
			{ TEXT("RoccoRippleRingSpacingUU"),        EKnobType::Float, TEXT("v14 §6: 'a short series of rings along the path'") },
			{ TEXT("RoccoRippleRingRadiusUU"),         EKnobType::Float, TEXT("v14 §6: ring size") },
			{ TEXT("RoccoRippleStartRingColor"),       EKnobType::Struct,TEXT("v14 §6: 'the starting ring in a different colour'") },
			{ TEXT("RoccoRippleTrailRingColor"),       EKnobType::Struct,TEXT("v14 §6: every ring after the first") },

			// --- v14 §6: Chut -------------------------------------------------------------------------
			{ TEXT("ChutKnifeFrontDamage"),            EKnobType::Float, TEXT("v14 §6: 50 from the front vs the standard 30") },
			{ TEXT("ChutKnifeBackDamage"),             EKnobType::Float, TEXT("v14 §6 ASSUMPTION: back damage stays 100") },
			{ TEXT("ChutBashKnockbackSpeed"),          EKnobType::Float, TEXT("v14 §6: bash — NO EFFECT ON THE CORE CARRIER (Control class)") },
			{ TEXT("ChutBashUpBias"),                  EKnobType::Float, TEXT("v14 §6: vertical component of the bash") },
			{ TEXT("ChutBashEndFraction"),             EKnobType::Float, TEXT("v14 §6: the END of his standard dash, not the whole dash") },
			{ TEXT("ChutBashRadiusUU"),                EKnobType::Float, TEXT("v14 §6: bash reach") },
			{ TEXT("ChudDamageReduction"),             EKnobType::Float, TEXT("v14 §6: 30% less from body shots and melees") },
			{ TEXT("ChudDurationSeconds"),             EKnobType::Float, TEXT("v14 §6: 10s, does not stack") },
			{ TEXT("ChudCooldownSeconds"),             EKnobType::Float, TEXT("v14 §6: 20s") },
			{ TEXT("bChudRefreshesOnKnifeKill"),       EKnobType::Bool,  TEXT("v14 §6: 'the timer refreshes on a knife kill'") },

			// --- v14 §6: Mace -------------------------------------------------------------------------
			{ TEXT("MaceMagnetRadiusBonus"),           EKnobType::Float, TEXT("v14 §6: +30% of CoreCatchRadius (450 -> 585). DERIVED, not hardcoded") },
			{ TEXT("MaceSuspendMaxSeconds"),           EKnobType::Float, TEXT("v14 §6: hold V in the air, up to 1.25s") },
			{ TEXT("MaceSuspendLateralSpeedCap"),      EKnobType::Float, TEXT("v14 §6: lateral movement capped at 550 uu/s while suspended") },
			{ TEXT("MaceSuspendCooldownSeconds"),      EKnobType::Float, TEXT("v14 §6: UNSPECIFIED; shipped at 0") },
			{ TEXT("MaceSpikeRangeUU"),                EKnobType::Float, TEXT("v14 §6: 'a medium distance'") },
			{ TEXT("MaceSpikeTravelSpeed"),            EKnobType::Float, TEXT("v14 §6: how fast the spike reaches its wall") },
			{ TEXT("MaceSpikeMaxSurfaceNormalZ"),      EKnobType::Float, TEXT("v15 §6: largest |Normal.Z| still called a wall — half of the inconsistency fix") },
			{ TEXT("MaceSpikeTraceRadiusUU"),          EKnobType::Float, TEXT("v15 §6: forgiveness sweep radius — the other half of the inconsistency fix") },
			{ TEXT("MaceSpikeEmbedSeconds"),           EKnobType::Float, TEXT("v14 §6: embeds in a WALL for 2s") },
			{ TEXT("MaceSpikePullSpeedMultiplier"),    EKnobType::Float, TEXT("v15 §6: 2.0 — the pull became the slow part once the range tripled") },
			{ TEXT("MaceSpikeArriveRadiusUU"),         EKnobType::Float, TEXT("v14 §6: how close ends the pull") },
			{ TEXT("MaceSpikeCooldownSeconds"),        EKnobType::Float, TEXT("v14 §6 ASSUMPTION: 20s, unspecified in the doc") },

			// --- v14 §6: Oyster -----------------------------------------------------------------------
			{ TEXT("OysterJarLifetimeSeconds"),        EKnobType::Float, TEXT("v14 §6: jars last 4s on the ground") },
			{ TEXT("OysterMaxJars"),                   EKnobType::Int,   TEXT("v14 §6: max 3; a fourth despawns the oldest") },
			{ TEXT("OysterJarBreakRadiusUU"),          EKnobType::Float, TEXT("v14 §6: an enemy TOUCHING a jar breaks it") },
			{ TEXT("OysterPoisonDamagePerTick"),       EKnobType::Float, TEXT("v14 §6: 3 damage — §4 names this as a carrier risk") },
			{ TEXT("OysterPoisonTickIntervalSeconds"), EKnobType::Float, TEXT("v14 §6: every 0.5s") },
			{ TEXT("OysterPoisonDurationSeconds"),     EKnobType::Float, TEXT("v14 §6: for 4s") },
			{ TEXT("OysterPoisonSlowFraction"),        EKnobType::Float, TEXT("v14 §6: -30% speed. CONTROL — refused on a carrier per the §4 assumption") },
			{ TEXT("OysterPoisonRadiusUU"),            EKnobType::Float, TEXT("v14 §6: 'poisoning nearby enemies'") },
			{ TEXT("OysterJarJumpZVelocity"),          EKnobType::Float, TEXT("v14 §6: jumping on his own jar breaks it and boosts him") },
			{ TEXT("OysterJarJumpRadiusUU"),           EKnobType::Float, TEXT("v14 §6: how close counts as stood on it") },
			{ TEXT("OysterPicklerDamage"),             EKnobType::Float, TEXT("v14 §6: 30 in an area — §4 names this as a carrier risk") },
			{ TEXT("OysterPicklerDamageRadiusUU"),     EKnobType::Float, TEXT("v14 §6: the area the 30 covers") },
			{ TEXT("OysterPicklerPullRadiusUU"),       EKnobType::Float, TEXT("v14 §6: 'a small radius'. CONTROL") },
			{ TEXT("OysterPicklerPullSpeed"),          EKnobType::Float, TEXT("v14 §6: pulls enemies toward it. CONTROL") },
			{ TEXT("OysterPicklerThrowSpeed"),         EKnobType::Float, TEXT("v14 §6: it is LOBBED, and it does NOT explode — it persists as a jar") },
			{ TEXT("OysterPicklerThrowUpBias"),        EKnobType::Float, TEXT("v14 §6: the arc on the lob") },
			{ TEXT("OysterPicklerCooldownSeconds"),    EKnobType::Float, TEXT("v14 §6 ASSUMPTION: 20s, unspecified in the doc") },

			// --- v14 §6: X ----------------------------------------------------------------------------
			{ TEXT("XBeeCount"),                       EKnobType::Int,   TEXT("v14 §6: five mechanical bees") },
			{ TEXT("XBeeOrbitRadiusUU"),               EKnobType::Float, TEXT("v14 §6 ASSUMPTION: bees hit on contact; X's body is the delivery") },
			{ TEXT("XBeeOrbitSpeedDegPerSecond"),      EKnobType::Float, TEXT("v14 §6: orbit rate") },
			{ TEXT("XBeeHitRadiusUU"),                 EKnobType::Float, TEXT("v14 §6: a bee's own touch radius") },
			{ TEXT("XVulnerableDurationSeconds"),      EKnobType::Float, TEXT("v14 §6: 2s, and a new application RESETS it. 'Does not stack' is SUPERSEDED by v16 §4 — one deadline, N stacks") },
			{ TEXT("XVulnerableDamageBonus"),          EKnobType::Float, TEXT("v24 §9: +35% from all sources (was +25%) — since v16 §4 this is the FIRST stack only") },
			{ TEXT("XVulnerableStackBonus"),           EKnobType::Float, TEXT("v16 §4: 'each additional stack only adds 5%'") },
			{ TEXT("XVulnerableMaxStacks"),            EKnobType::Int,   TEXT("v16 §4 ASSUMPTION: cap the stacks. 5 = XBeeCount = XStingBulletCount, so x1.45 is the ceiling") },
			{ TEXT("XVulnerableSpeedBonus"),           EKnobType::Float, TEXT("v14 §6: +10% while ANY enemy is vulnerable") },
			{ TEXT("XStingCooldownSeconds"),           EKnobType::Float, TEXT("v14 §6: 25s — the one that is not 20") },
			{ TEXT("XStingBulletCount"),               EKnobType::Int,   TEXT("v14 §6: the next five bullets. Keep equal to XBeeCount") },

			// =========================================================================================
			// SPEC v18 §2 — ROXIE, ELLE AND SLIMEBALL, EVERY TUNING VALUE THEY WILL NEED
			//
			// *** THESE ROWS EXIST BEFORE THE ABILITIES DO, AND THAT IS THE POINT OF THE PASS. ***
			// Three character agents write these three characters IN PARALLEL after this lands, and not
			// one of them may edit UTraceSettings — three simultaneous edits to one 4500-line header is
			// three merge conflicts and a knob quietly lost in the resolution. So every number spec v18
			// §2 names, plus every number an implementation of it obviously needs (a projectile's
			// lifetime, a slow's linger, a wall's third dimension), is declared here first.
			//
			// Which means this table is the ONLY thing standing between those agents and a silent
			// no-op: they will read these by name, a misspelling is not a build error, and a
			// misspelled read returns a default-constructed nothing. The v8 rule — "a brand-new
			// mechanic's knobs are listed on the pass that introduces them, not the pass after" — is
			// doing more work here than it has ever done.
			//
			// WHAT THIS TABLE STILL CANNOT PROVE, said again because three passes are about to depend
			// on it: it proves a property EXISTS, is `config` (so DefaultGame.ini reaches it) and is
			// EditAnywhere (so the panel does). It cannot prove anybody READS it. Until the three
			// character files land, every row below says OK while the panel moves nothing — exactly
			// the v12 §6 wall-fit situation, and it was right about that one too.
			//
			// THE CARRIER RULE IS NOT A KNOB, AND NOTHING BELOW IS ONE. Roxie's flat 100, Elle's
			// teleport and Slimeball's 35% slow are the three most dangerous additions this game has
			// taken; each of them goes through UTraceAbilityComponent::CanAffectTarget, and there is
			// deliberately no row here that could switch that off.
			// =========================================================================================

			// --- v18 §1a: the air-reversal brake ------------------------------------------------------
			//
			// The one MOVEMENT knob this pass adds, listed with the v18 block rather than the air block
			// above it for the v8 reason: a brand-new mechanic's knob belongs on the pass that
			// introduces it. It shipped for part of this pass with no property at all, and the movement
			// component's own MOVEKNOB report said so out loud every run —
			//     MOVEKNOB AirStrafeOpposingDeceleration FALLBACK ... (property missing -> ini CANNOT
			//     tune it)
			// — which is the failure mode this whole table exists to make loud instead of silent. The
			// component ran on its own built-in 2200 the entire time, so the FIX changed no number a
			// player can feel; it changed whether a designer can retune it without a rebuild.
			{ TEXT("AirStrafeOpposingDeceleration"),   EKnobType::Float, TEXT("v18 §1a: uu/s^2 bled at a DEAD 180 reversal, scaled by the negative part of dot(wish, travel) so it is EXACTLY 0 at 90 degrees and inside it - the air strafe is untouched [by-name bind]") },

			// --- v18 §2: Roxie -----------------------------------------------------------------------
			//
			// "Tuning to come after first implementation - so make every part of it a knob and do not
			// agonise over the values" is the doc's own instruction for the rocket, and the values below
			// are first guesses that are meant to be moved. The two that are NOT guesses are the ones
			// the doc states outright: 100 damage and the two cooldowns.
			{ TEXT("RoxieJumpHeightBonus"),            EKnobType::Float, TEXT("v18 §2: 'jumps 15% higher'. A HEIGHT fraction - velocity scales as sqrt(1+this) = 1.0724, NOT 1.15. See the header") },
			{ TEXT("RoxieRocketDamage"),               EKnobType::Float, TEXT("v18 §2: flat 100 'anywhere on the body', no headshot zone. DAMAGE - never reaches a carrier") },
			{ TEXT("RoxieRocketSpeed"),                EKnobType::Float, TEXT("v18 §2: how fast the rocket travels; slow enough to be dodged is the point of the wobble") },
			{ TEXT("RoxieRocketLifetimeSeconds"),      EKnobType::Float, TEXT("v18 §2: a projectile needs an end. Speed x this is the effective range") },
			{ TEXT("RoxieRocketHitRadiusUU"),          EKnobType::Float, TEXT("v18 §2: the rocket's OWN radius on top of the victim capsule. Not a splash radius - there is no splash") },
			{ TEXT("RoxieRocketWobbleAmplitudeUU"),    EKnobType::Float, TEXT("v18 §2: 'wobbles in flight, deliberately inaccurate'. 0 = a straight, easily-aimed rocket = the RED arm") },
			{ TEXT("RoxieRocketWobbleFrequencyHz"),    EKnobType::Float, TEXT("v18 §2: wobbles per second. With the amplitude, this is the whole 'hard to aim'") },
			{ TEXT("RoxieRocketSelfLaunchImpulse"),    EKnobType::Float, TEXT("v18 §2: 'launches her backwards, fast and far'. Applied to ROXIE, opposite her aim") },
			{ TEXT("RoxieRocketSelfLaunchUpBias"),     EKnobType::Float, TEXT("v18 §2: fraction of the impulse sent upward - 'far' needs air time, same shape as ChutBashUpBias") },
			{ TEXT("RoxieRocketCooldownSeconds"),      EKnobType::Float, TEXT("v18 §2: 35s, stated. SEPARATE from the E cooldown - this is the V ability") },
			{ TEXT("RoxieModdedFireRateMultiplier"),   EKnobType::Float, TEXT("v18 §2: x1.65 fire rate. DIVIDES FireInterval; it is a RATE MULTIPLIER ON THE BASE (spec v24 §0), never an interval or an RPM") },
			{ TEXT("bRoxieModdedFullAuto"),            EKnobType::Bool,  TEXT("v18 §2: 'the gun becomes full auto' for the duration") },
			{ TEXT("RoxieModdedDurationSeconds"),      EKnobType::Float, TEXT("v18 §2: 5s, 'one clip OR 5 seconds, whichever comes first'") },
			{ TEXT("bRoxieModdedEndsOnReload"),        EKnobType::Bool,  TEXT("v18 §2 ASSUMPTION: 'one clip' = the clip loaded when Modded started, so reloading ends it") },
			{ TEXT("RoxieModdedCooldownSeconds"),      EKnobType::Float, TEXT("v18 §2: 25s. THE ENFORCED number - the card prints 25 too and Trace.VerifyCharacterData compares them") },

			// --- v18 §2: Elle ------------------------------------------------------------------------
			{ TEXT("ElleCloakDurationSeconds"),        EKnobType::Float, TEXT("v18 §2: 3s of cloak after passing or throwing THE CORE (ASSUMPTION: 'the trace' means the Core)") },
			{ TEXT("ElleCloakOpacity"),                EKnobType::Float, TEXT("v18 §2: 'semi-transparent and hard to see or aim at'. 0 = invisible, 1 = no cloak at all = the RED arm") },
			{ TEXT("bElleCloakEndsOnCorePickup"),      EKnobType::Bool,  TEXT("v18 §2 ASSUMPTION: picking the Core back up drops the cloak early") },
			{ TEXT("ElleSlideJumpGainBonus"),          EKnobType::Float, TEXT("v18 §2: +40% of the GAIN, not the multiplier. 0.446875 x 1.40 = 0.625, so hers is 1.625 vs everyone's 1.446875") },
			{ TEXT("ElleSnapSecondGateWindowSeconds"), EKnobType::Float, TEXT("v18 §2: 4s to place the second gate; miss it and the first expires") },
			{ TEXT("ElleSnapPairLifetimeSeconds"),     EKnobType::Float, TEXT("v18 §2: with both placed, both expire after 8s") },
			{ TEXT("ElleSnapGateRadiusUU"),            EKnobType::Float, TEXT("v18 §2: how close counts as stepping into a gate") },
			{ TEXT("ElleSnapTeleportLockoutSeconds"),  EKnobType::Float, TEXT("v18 §2: per-player re-entry lockout. 0 ping-pongs a player between the two gates forever = the RED arm") },
			{ TEXT("bElleSnapUsableByBothTeams"),      EKnobType::Bool,  TEXT("v18 §2 ASSUMPTION, THE MOST REVERSIBLE DECISION IN THE DOC: 'players' read as EITHER team, on Ripple's precedent") },
			{ TEXT("bElleSnapCarrierMayUseGate"),      EKnobType::Bool,  TEXT("v18 §2: may a CARRIER step through VOLUNTARILY. Being moved by an ENEMY gate is Control and is refused by the choke point either way - that half has no knob") },
			{ TEXT("ElleSnapCooldownSeconds"),         EKnobType::Float, TEXT("v18 §2: 35s. THE ENFORCED number; the card prints 35 too") },

			// --- v18 §2: Slimeball -------------------------------------------------------------------
			{ TEXT("SlimeballWallStickMaxSurfaceNormalZ"), EKnobType::Float, TEXT("v18 §2: largest |normal.Z| still called a wall. 0.70 = the walkable limit, i.e. 'if he cannot stand on it he can stick to it'. Same reasoning as MaceSpikeMaxSurfaceNormalZ, deliberately a SEPARATE knob") },
			{ TEXT("SlimeballWallStickRangeUU"),       EKnobType::Float, TEXT("v18 §2: how close to a wall he must be for hold-V to grab it") },
			{ TEXT("SlimeballWallStickSlideSpeed"),    EKnobType::Float, TEXT("v18 §2: uu/s he creeps down while stuck. 0 = welded in place, which is what 'sticks' says") },
			{ TEXT("SlimeballWallStickMaxSeconds"),    EKnobType::Float, TEXT("v18 §2: UNSPECIFIED; shipped at 0 = as long as V is held, holding is the whole cost") },
			{ TEXT("SlimeballWallStickCooldownSeconds"), EKnobType::Float, TEXT("v18 §2: UNSPECIFIED; shipped at 0, same reasoning as MaceSuspendCooldownSeconds") },
			{ TEXT("SlimeballStuckFireRateBonus"),     EKnobType::Float, TEXT("v18 §2: +30% fire rate WHILE STUCK ONLY. A RATE bonus - it divides FireInterval") },
			{ TEXT("SlimeballStuckDamageReduction"),   EKnobType::Float, TEXT("v18 §2: 30% off BODY SHOTS AND FRONT KNIFE STABS while stuck. ASSUMPTION: headshots and backstabs unreduced, exactly like Chud") },
			{ TEXT("SlimewallHeightUU"),               EKnobType::Float, TEXT("v18 §2: 'one player height tall' = 176uu (2 x the 88 capsule half-height). CHANGEABLE, the doc says so explicitly") },
			{ TEXT("SlimewallWidthUU"),                EKnobType::Float, TEXT("v18 §2: the slab's THICKNESS along his aim, i.e. how far an enemy walks through it. 176 = the doc's 'and wide'") },
			{ TEXT("SlimewallLengthUU"),               EKnobType::Float, TEXT("v18 §2: the SPAN across his aim - the part you hide behind. 1100 = the arena's one-player-height cover block") },
			{ TEXT("SlimewallRangeUU"),                EKnobType::Float, TEXT("v18 §2: how far in front of him it goes up") },
			{ TEXT("SlimewallSlowFraction"),           EKnobType::Float, TEXT("v18 §2: 35%. CONTROL - refused on a carrier per the §4 assumption. 0 = a wall that only blocks sight = the RED arm") },
			{ TEXT("SlimewallSlowLingerSeconds"),      EKnobType::Float, TEXT("v18 §2 ASSUMPTION: how long the slow lasts after leaving the slab; 0 makes a 176uu-thick wall barely noticeable") },
			{ TEXT("SlimewallDurationSeconds"),        EKnobType::Float, TEXT("v18 §2: 4s, stated") },
			{ TEXT("SlimewallOpacity"),                EKnobType::Float, TEXT("v18 §2: 'obstructs vision'. This is the LOOK only - it must never be given blocking collision, bullets pass through") },
			{ TEXT("bSlimewallSlowsOwnTeam"),          EKnobType::Bool,  TEXT("v18 §2 ASSUMPTION: false - it does not slow Slimeball or his team") },
			{ TEXT("SlimewallCooldownSeconds"),        EKnobType::Float, TEXT("v18 §2: 25s. THE ENFORCED number; the card prints 25 too") },
		};

		const UTraceSettings& Table = UTraceSettings::Get();
		int32 BoundCount = 0;
		int32 DeadCount = 0;

		for (const FKnobSpec& Knob : Knobs)
		{
			const FName KnobName(Knob.Name);

			// Which class, and which object to read the value out of. Null OwnerPath is this page,
			// which is every knob before spec v10 and most of them after it. A named owner is looked
			// up by path so this file never has to include another settings header - see FKnobSpec.
			const UClass* OwnerClass = UTraceSettings::StaticClass();
			const UObject* ContainerObject = &Table;

			if (Knob.OwnerPath != nullptr)
			{
				OwnerClass = FindObject<UClass>(nullptr, Knob.OwnerPath);
				ContainerObject = (OwnerClass != nullptr) ? OwnerClass->GetDefaultObject() : nullptr;
			}

			const void* Container = ContainerObject;

			if (OwnerClass == nullptr || Container == nullptr)
			{
				++DeadCount;
				UE_LOG(LogTraceGame, Error,
					TEXT("[VerifyKnobs]   DEAD %-34s  owning class '%s' does not exist in this build  (%s)"),
					Knob.Name, Knob.OwnerPath, Knob.Note);
				continue;
			}

			const FProperty* Found = OwnerClass->FindPropertyByName(KnobName);

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
						Value = FString::Printf(TEXT("%.4f"), AsFloat->GetPropertyValue_InContainer(Container));
					}
					break;
				case EKnobType::Bool:
					if (const FBoolProperty* AsBool = CastField<FBoolProperty>(Found))
					{
						bTypeOk = true;
						Value = AsBool->GetPropertyValue_InContainer(Container) ? TEXT("true") : TEXT("false");
					}
					break;
				case EKnobType::Int:
					if (const FIntProperty* AsInt = CastField<FIntProperty>(Found))
					{
						bTypeOk = true;
						Value = FString::Printf(TEXT("%d"), AsInt->GetPropertyValue_InContainer(Container));
					}
					break;
				case EKnobType::Enum:
					if (CastField<FEnumProperty>(Found) != nullptr || CastField<FByteProperty>(Found) != nullptr)
					{
						bTypeOk = true;

						// ExportText rather than reading the underlying byte: the ENUMERATOR NAME is
						// what Config/DefaultGame.ini serialises ("BotDifficulty=Normal"), so printing
						// the name is what lets a reader compare this line against the ini directly.
						//
						// Reset() first — ExportText_InContainer APPENDS to the string it is handed,
						// and Value still carries the "<unbound>" placeholder the other branches
						// overwrite wholesale.
						Value.Reset();
						Found->ExportText_InContainer(0, Value, Container, Container, nullptr, PPF_None);
					}
					break;
				case EKnobType::Struct:
					// Same ExportText treatment as Enum, and for the same reason: what a struct knob
					// round-trips through the ini is its TEXT form — "(R=1.000000,G=0.550000,...)" —
					// so printing that form is what lets this line be compared against DefaultGame.ini
					// directly. A colour that failed to parse comes back as the header default and
					// looks perfectly healthy in a debugger.
					if (const FStructProperty* AsStruct = CastField<FStructProperty>(Found))
					{
						bTypeOk = true;
						Value.Reset();
						AsStruct->ExportText_InContainer(0, Value, Container, Container, nullptr, PPF_None);
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

				// The owning OBJECT is named, not just the class. Spec v10 is the first pass with
				// knobs on more than one settings page, and the failure mode that costs a whole pass
				// is reading the RIGHT property's offset out of the WRONG object: every value comes
				// back plausible and wrong. Printing what was read from makes that impossible to miss.
				UE_LOG(LogTraceGame, Display, TEXT("[VerifyKnobs]   OK   %-34s = %-10s  [%s]  (%s)"),
					Knob.Name, *Value, *GetNameSafe(ContainerObject), Knob.Note);
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
