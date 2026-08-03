#include "TraceSettings.h"

#include "Kismet/GameplayStatics.h"     // UGameplayStatics::ParseOption
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "Trace.h"                      // LogTraceGame
#include "TraceTypes.h"

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
	// where 3000uu is most of the pitch; on 24000 x 12000 it is close quarters. Measured with the
	// player walked up the field, bots had them inside SightRange for 69.7% of bot-ticks but inside
	// MaxEngagementRange with line of sight for 16.0% — they could see the player fine and simply
	// were not allowed to shoot. Meanwhile HitscanRange is 28000, so the player could shoot back
	// from anywhere. That asymmetry is indistinguishable from "the bots ignore me".
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
