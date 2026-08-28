// Trace — LILY. See the header for the spec v19 §3 reading, for the mid-Zip halving clause and the
// misreading it guards against, and for the 20 Hz limitation of driving flight from an ability set.

#include "Abilities/Characters/TraceAbilitySetLily.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"           // the FX harness writes its own frames
#include "Misc/Paths.h"
#include "UnrealClient.h"                         // FScreenshotRequest — the before/after pair


#include "Components/AudioComponent.h"                    // the §1.6.4 flight loop's handle
#include "Components/InstancedStaticMeshComponent.h"       // one component per bead ring (§1.4 budget)
#include "Components/CapsuleComponent.h"                  // the LIVE capsule half height (feet / chest)
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"                  // ETraceCharacterId::Lily — the id, not the colour
#include "Audio/TraceAudio.h"                             // Play / StartLoopOn
#include "Audio/TraceSoundEvents.h"                       // LilyZip, LilyZipLoop
#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterRoster.h"                    // THE accent. See LilyAccent() below.
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceFxShapes.h"                       // the shared primitives, MIDs and units
#include "Gameplay/TraceHealthComponent.h"                // the FX harness kills her mid-flight
#include "Movement/TraceCharacterMovementComponent.h"
#include "Settings/TraceUserSettings.h"                   // the player's Jump / Crouch binds
#include "Trace.h"
#include "TraceSettings.h"

#define LOCTEXT_NAMESPACE "TraceLily"

// =================================================================================================
// THE RED ARMS. One per rule, each removing that rule and nothing else. Same shape and the same
// reasoning as TraceAbilitySetChut.cpp's three.
// =================================================================================================

/**
 * *** THE ONE THE SUBTLE CLAUSE HANGS ON. *** 0 removes BOTH Core halvings — the one at the cast and
 * the one on a mid-flight pickup — and nothing else, so a Zip is always its full 5 s. That is the
 * arm Trace.Lily.ZipVerify runs first: without it, "the remaining duration halved to 0.5 s" is a
 * sentence about a number nobody has watched fail to halve.
 */
static TAutoConsoleVariable<int32> CVarLilyZipCoreHalving(
	TEXT("Trace.Lily.ZipCoreHalving"),
	1,
	TEXT("Dev/red arm. 1 (default) = carrying the Core halves Zip's duration at the cast, and picking the Core "
	     "up mid-flight halves WHAT IS LEFT (spec v19 §3). 0 = neither halving happens, so both assertions in "
	     "Trace.Lily.ZipVerify must go red. NEVER SHIP 0."),
	ECVF_Cheat);

/** 0 makes Zip a no-op that still costs the cooldown, so every flight assertion must go red. */
static TAutoConsoleVariable<int32> CVarLilyZip(
	TEXT("Trace.Lily.Zip"),
	1,
	TEXT("Dev/red arm. 1 (default) = E flies her for 5s (spec v19 §3). 0 = the press is accepted, the cooldown "
	     "starts, and she does not fly."),
	ECVF_Cheat);

/**
 * *** THE RED ARM FOR DEMO 19 ITEM 4. *** 0 restores, exactly, the code the user complained about.
 *
 * With 0 the climb is driven by the old bJumpHeld latch and nothing else: the key poll does not run,
 * so nothing ever clears it, so one press of space climbs for the rest of the flight and crouch can
 * never win the `!climbing && IsCrouchHeld()` test. That is not an imitation of the bug — it is the
 * shipped v21 code path, selected by an if.
 *
 * Trace.Lily.KeyTest runs this arm FIRST and refuses to grade the fix unless it reproduces. NEVER
 * SHIP 0.
 */
/**
 * *** THE RED ARM FOR DEMO 19 ITEM 8. *** 0 restores the shipped v21 rule: the extra charge always.
 *
 * With 0, GetExtraDashCharges() stops asking whether she is carrying, so she is back to two dashes
 * free-running and THREE while carrying — which is what the character-select card used to promise and
 * what Trace.Lily.DashTest's red arm has to reproduce before the green reading is worth anything.
 * NEVER SHIP 0.
 */
static TAutoConsoleVariable<int32> CVarLilyDashCarrierGate(
	TEXT("Trace.Lily.DashCarrierGate"),
	1,
	TEXT("Dev/red arm. 1 (default) = Demo 19 item 8: Lily's extra dash charge applies ONLY while she is NOT "
	     "carrying the Core (2 free-running, 2 carrying). 0 = the shipped v21 rule, the charge always (2 "
	     "free-running, 3 carrying). NEVER SHIP 0."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarLilyZipHoldRelease(
	TEXT("Trace.Lily.ZipHoldRelease"),
	1,
	TEXT("Dev/red arm. 1 (default) = the climb follows the jump key as a LEVEL, polled on the machine with the "
	     "keyboard and carried to the server on FLAG_Custom_1, so letting go stops the climb and crouch then "
	     "descends. 0 = the shipped v21 behaviour: the press latches, the release never arrives (the framework's "
	     "release hook has no caller), she climbs for the whole flight and crouch does nothing. NEVER SHIP 0."),
	ECVF_Cheat);

// =================================================================================================
// FX_AUDIO_PLAN §2.1 — THE NUMBERS, IN ONE PLACE
//
// NAMED, not anonymous: this module builds as a unity blob and "AuraRingRadiusUU" is a name three
// other character files would also want. Same reasoning as TraceLilyVerifyFile below.
// =================================================================================================
namespace TraceLilyFxFile
{
	/**
	 * Lily's accent — the ONE hue every piece of her flight wears (aura rings, cast ring, climb jet).
	 *
	 * *** READ FROM THE ROSTER, NOT COPIED. *** This was `const FLinearColor LilyIce(0.75f, 0.92f,
	 * 1.00f)`, a transcription of ART_BIBLE §2.3's #E1F6FF. The W5 re-space moved her to #B8F8FF —
	 * still ice, but a real ice with a saturation a player can see, where the old one was so close to
	 * white that the portrait check had to grant it an exemption — and this copy stayed behind. Her
	 * body then flew in one colour and her aura in another. A colour that lives in two files is a
	 * colour that will disagree with itself; this one now lives in one.
	 *
	 * HER BRIGHTEST CHANNEL IS STILL 1.0, so HeadroomGlowFor's ceiling is unchanged by the re-space
	 * and none of §2.1's measured glows had to move with it.
	 */
	FLinearColor LilyAccent()
	{
		if (const TraceCharacterRoster::FTraceCharacterEntry* Row =
			TraceCharacterRoster::Find(static_cast<uint8>(ETraceCharacterId::Lily)))
		{
			return FLinearColor(Row->Accent.R, Row->Accent.G, Row->Accent.B, 1.f);
		}
		return FLinearColor::White;
	}

	/**
	 * §2.1 flight aura: "2 rings (cylinder, r 46 uu, h 3 uu, additive ice I 0.4)".
	 *
	 * The RADIUS is the doc's, unchanged and inside §1.4's 96 uu capsule footprint. The other two
	 * numbers are not: "cylinder" became a bead ring (see the header's note on AuraRingA — a solid
	 * engine cylinder is a filled disc, not a shell) and the additive intensity became an emissive
	 * headroom (see EmissiveHueHeadroom). Both are recorded as deviations in the report; both were
	 * forced by a capture rather than chosen.
	 */
	constexpr float AuraRingRadiusUU = 46.f;

	/**
	 * *** THE BIBLE'S THIN-EMISSIVE FLOOR, AND WHY IT IS ON THE BEAD RATHER THAN ON A BAND. ***
	 *
	 * §2.1 asks for h 3 uu rings and an h 4 uu cast ring. ART_BIBLE §3.4 forbids sub-8 uu emissive
	 * outright outside first-person range: at 1920x1080 / 90% scale one screen pixel is 3.8 uu at
	 * 3,000 uu, and TSR dissolves anything under ~2.2 px into a dotted line — the exact failure
	 * visual-audit.md §2.4 recorded. A 3 uu band on a flying player IS a world-readable emissive at
	 * 3,000 uu, so it loses.
	 *
	 * The rings are beads rather than bands (see the header), so the floored dimension is the bead's
	 * DIAMETER: MinBeadRadiusUU below is 4 uu, i.e. 8 uu across, which is the same floor and the same
	 * arithmetic W3-FXBURST applied to its own ring beads (MinEmissiveRadiusUU = 4).
	 */

	/**
	 * §2.1: each ring "drifts to feet over 0.7 s and rewraps". The two rings share ONE path and are
	 * half a period apart, which is what makes the wash continuous.
	 *
	 * §2.1's literal reading is "spawn at chest (+40 uu) and hips". Rejected, and here is why: the
	 * chest->feet path is 128 uu and chest->hips is 40 uu of it, so a pair pinned to those two
	 * heights travels 40 uu apart and then leaves an 88 uu hole every wrap — a clumped pair with a
	 * visible gap, not a "continuous anti-gravity wash", which is what the same row asks for in its
	 * own last four words. Half a period apart puts the second ring at mid-thigh at t = 0 instead of
	 * at the hips, and the wash has no gap at any moment.
	 */
	constexpr float AuraTravelSeconds = 0.7f;

	/** Where the wash starts, as a fraction of the LIVE capsule half height. 0.45 x 88 = 40 uu. */
	constexpr float ChestFractionOfHalfHeight = 0.45f;

	/**
	 * §2.1 cast flash: "cylinder shell r 40 -> 150 uu (h 4 uu), additive ice I 0.5 -> 0, 0.4 s".
	 * Radii and duration verbatim; the "I 0.5 -> 0" fade rides the bead SIZE instead, because an
	 * opaque bead dimmed to zero is a black bead rather than no bead (see LayOutRing).
	 */
	constexpr float CastRingStartRadiusUU = 40.f;
	constexpr float CastRingEndRadiusUU = 150.f;
	constexpr float CastRingSeconds = 0.4f;

	/** §2.1 climb jet: "inverted cone under the capsule (base r 22 uu, h 60 uu, apex down), I 0.35". */
	constexpr float JetBaseRadiusUU = 22.f;
	constexpr float JetHeightUU = 60.f;
	constexpr float JetIntensity = 0.35f;

	/** §2.1: "visible only while ascending (VelZ > 60 uu/s — read from the pawn, no extra replication)". */
	constexpr float ClimbVelocityThresholdUU = 60.f;

	/** §2.1 end dissolve: "aura rings fade I->0 over 0.3 s (never pop-out, bible §6.4)". */
	constexpr float DissolveSeconds = 0.3f;

	/** The §1.6.4 loop's fades. The out is the value TraceAudio.h documents for the off-edge. */
	constexpr float LoopFadeInSeconds = 0.15f;
	constexpr float LoopFadeOutSeconds = 0.25f;

	/** §1.4's ceiling, restated where it is spent so a fifth piece cannot be added without seeing it. */
	constexpr int32 MaxAttachedPrimitives = 4;

	/**
	 * Beads per ring. 24, and the number is chosen with the 13% bead radius below: 2*PI*R/24 is
	 * 0.262*R of spacing and a 13% bead is 0.26*R across, so the ring is continuous at ANY radius —
	 * which is what the cast flash needs while it grows from 40 uu to 150 uu.
	 */
	constexpr int32 BeadsPerRing = 24;

	/** Bead radius as a fraction of the ring's. Same proportion W3-FXBURST measured for its rings. */
	constexpr float BeadRadiusFraction = 0.13f;

	/** The bible §3.4 floor, as a RADIUS: 4 uu radius is 8 uu across. */
	constexpr float MinBeadRadiusUU = 4.f;

	float BeadRadiusFor(float RingRadiusUU)
	{
		return FMath::Max(MinBeadRadiusUU, RingRadiusUU * BeadRadiusFraction);
	}

	/**
	 * *** THE BEADS ARE EMISSIVE AND THE JET IS ADDITIVE, AND THAT SPLIT IS W3-FXBURST'S MEASURED
	 * RULE RATHER THAN THIS FILE'S OPINION: "BIG VOLUMES ARE ADDITIVE, THIN AND SMALL PIECES ARE
	 * EMISSIVE" (reports/W3-FXBURST.md finding 3). ***
	 *
	 * Two independent reasons, and this pass hit BOTH of them in one capture
	 * (frames-W4-KITS-A/TraceAutoShot_lily_cast_20260825_031544.png, twenty-four grey checkered
	 * spheres where an ice ring was specified):
	 *
	 *   1. THE ENGINE'S ADDITIVE MATERIAL IS NOT INSTANCED-SAFE. /Engine/EngineMaterials/
	 *      EmissiveMeshMaterial carries no `used_with_instanced_static_meshes` flag, so an
	 *      UInstancedStaticMeshComponent wearing it renders with the DEFAULT MATERIAL — the grey
	 *      checkerboard — and the editor's auto-repair for that flag is gated on not running as a
	 *      game, which every run of this project is. Scripts/generate_content.py sets the flag on
	 *      M_TraceNeon for exactly this reason and says so at line 431.
	 *   2. ADDITIVE CANNOT CARRY A HUE AT THIS SIZE ANYWAY. Additive intensity is clamped at 1.0, so
	 *      a 0.4-intensity bead over the arena's lit blue floor is the floor plus a little — which is
	 *      the "additive pieces came back GREY" half of the same W3 finding.
	 *
	 * §1.4 asks looping FX to be "additive only, intensity <= 0.5". That rule is protecting two
	 * things — do not occlude the pawn, do not overpower the frame — and a 12 uu bead at a
	 * headroom-capped glow does neither. Choosing grey over hue to honour the letter of it would
	 * break bible §6.2's "one hue per effect" instead, which is the louder rule. Recorded as a
	 * deviation in the report rather than smuggled in.
	 *
	 * THE HEADROOM is FxBurst's own measured 0.70: the glow is capped so the BRIGHTEST CHANNEL of
	 * Colour x Glow lands there — bright enough to clear the arena's bloom threshold, low enough that
	 * the other channels are not dragged up with it and every kit's ring turns into the same white
	 * band. Lily ice's brightest channel is 1.00 (blue), so her rings run at Glow 0.70.
	 */
	constexpr float EmissiveHueHeadroom = 0.70f;

	/** The Glow that puts @p Color's brightest channel exactly at the headroom. */
	float HeadroomGlowFor(const FLinearColor& Color)
	{
		const float Brightest = FMath::Max3(Color.R, Color.G, Color.B);
		return (Brightest > KINDA_SMALL_NUMBER) ? (EmissiveHueHeadroom / Brightest) : EmissiveHueHeadroom;
	}

	/** Destroys a piece and forgets its MID, in the one order that cannot leave a dangling MID. */
	template <typename ComponentType>
	void KillPiece(TObjectPtr<ComponentType>& Piece, TObjectPtr<UMaterialInstanceDynamic>& MID,
		ETraceFxBlend& Blend)
	{
		if (Piece != nullptr)
		{
			Piece->DestroyComponent();
		}
		Piece = nullptr;
		MID = nullptr;
		Blend = ETraceFxBlend::None;
	}

	/** The live half height, or the shipped default when the capsule is somehow not there. */
	float HalfHeightOf(const ATraceCharacter* Pawn)
	{
		// READ LIVE, never a copy: she crouches (slide) mid-flight, and a hard-coded 88 would put the
		// wash's floor 44 uu below a crouched pawn's actual feet.
		if (Pawn != nullptr && Pawn->GetCapsuleComponent() != nullptr)
		{
			return Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		}
		return 88.f;
	}
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetLily::OnEquipped()
{
	bZipping = false;
	ZipEndMatchTime = 0.f;
	bJumpHeld = false;
	ZipCorePickups = 0;

	// Seeded from the pawn rather than from false: she can be handed this character while ALREADY
	// holding the Core (a mid-match switch, the select screen's auto-pick), and a false seed would
	// read the very next tick as a pickup edge and halve a Zip she has not started.
	const ATraceCharacter* MyPawn = GetCharacter();
	bWasCarrier = (MyPawn != nullptr) && UTraceAbilityComponent::IsCarrier(MyPawn);
}

void UTraceAbilitySetLily::OnUnequipped()
{
	StopZip(TEXT("character changed"));

	// THE CHARACTER-SWAP HALF OF §1.2 OBLIGATION 2, and it is a different failure from the death
	// one: a swap destroys this ability set while the PAWN may well survive (mid-match switches keep
	// the body until the respawn). The four components are outered to that pawn, so nothing else
	// would ever reap them — the aura would hang on a Lily who is now a Rocco, driven by no tick.
	// IMMEDIATE, not the 0.3 s dissolve: there will be no tick left to finish a fade.
	DetachZipFx(/*bImmediate=*/true);
}

void UTraceAbilitySetLily::OnPawnSpawned()
{
	// A new pawn is new components. The old ones went with the old pawn (they were outered to it),
	// so the pointers here are already stale rather than leaked — clearing them is bookkeeping, not
	// cleanup, and DetachZipFx does both without caring which.
	DetachZipFx(/*bImmediate=*/true);

	// And then re-present whatever the replicated state says is ON. Normally nothing: the death wipe
	// clears Zipping (that is why the flag IS EffectActive). This is here for the case it does not
	// cover — a pawn swapped underneath a LIVING player — because "attach on respawn" is a §1.2
	// obligation and an obligation honoured only when it is convenient is not one.
	SyncClientFx(State());
}

void UTraceAbilitySetLily::OnPawnDied()
{
	// SPEC v19 §4.2. The central wipe (UTraceAbilityComponent::ApplyDeathStateWipe) already clears
	// the replicated half — that is why Zip's flag is TraceAbilityFlags::EffectActive and its
	// deadline is EffectEndMatchTime. This clears the LOCAL half on this machine, which the framework
	// cannot reach, so a dead Lily is not still being flown by her own owning client.
	//
	// The E COOLDOWN IS DELIBERATELY UNTOUCHED: "cooldown timers should still not reset when players
	// die, and should just continue ticking down."
	StopZip(TEXT("died"));

	// §1.2 obligation 2, the server's half. IMMEDIATE — a corpse must not wear a fading aura for a
	// third of a second. This hook is SERVER-ONLY (ATraceGameMode is its only caller), which is why
	// TickZipFx carries the same rule off Pawn->IsAlive() for the clients.
	DetachZipFx(/*bImmediate=*/true);
}

void UTraceAbilitySetLily::OnHalfTime()
{
	// THROUGH StopZip, NOT BY CLEARING THE FIELDS. The framework has normally already Reset() the net
	// state by the time this runs, so writing bZipping = false here would look equivalent — but it is
	// only equivalent because of something another file does first. StopZip publishes the cleared
	// flag itself, which makes this correct on its own and keeps the "flying" answer and the "not
	// flying" fields from ever disagreeing. (That is not hypothetical: Trace.Lily.ZipVerify calls
	// this directly, between arms, with no framework reset in front of it.)
	StopZip(TEXT("half time"));

	bJumpHeld = false;
	ZipCorePickups = 0;
	bWasCarrier = false;

	// The interval is a dead phase and the framework Reset()s the net state into it, so the falling
	// edge that would normally dissolve the aura may or may not be delivered. Immediate here for the
	// same reason the fields above are cleared by hand: half time must leave nothing running.
	DetachZipFx(/*bImmediate=*/true);
}

bool UTraceAbilitySetLily::ShouldDriveMovement() const
{
	// A simulated proxy's velocity is replicated; writing it there would fight the interpolation and
	// show up as somebody else's Lily stuttering in the air.
	return HasAuthority() || IsLocallyControlled();
}

// =================================================================================================
// MOVEMENT + PASSIVE — the three numbers owned by other slices
// =================================================================================================

int32 UTraceAbilitySetLily::GetExtraDashCharges() const
{
	// DEMO 19 ITEM 8: "only ... when she is not carrying the core".
	//
	// THE CONDITION IS ON THE ADDEND, NOT ON THE TOTAL, and that distinction is the whole item.
	// UTraceCharacterMovementComponent::GetMaxDashCharges() has already added the carrier's own extra
	// charge by the time it asks this function, so returning 0 while carrying leaves her on the same
	// two dashes as everybody else who is carrying — it does not take her down to one. Returning
	// "2 minus whatever" here instead would have.
	//
	// READ THROUGH UTraceAbilityComponent::IsCarrier, which ORs the pawn's replicated mirror with the
	// Core's own holder, so a client answers the same as the server on the frame of a pickup. That
	// matters more here than it does for the duration halvings: this value is read inside the
	// PREDICTED movement path, and a client that disagreed for a frame would spend a charge the
	// server then refunded.
	const ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn != nullptr && UTraceAbilityComponent::IsCarrier(MyPawn)
		&& CVarLilyDashCarrierGate.GetValueOnAnyThread() != 0)
	{
		return 0;
	}

	return FMath::Clamp(UTraceSettings::Get().LilyExtraDashCharges, 0, 5);
}

float UTraceAbilitySetLily::GetMaxHealthOverride() const
{
	// Floored at 1 rather than at 0: 0 is the sentinel for "this character has no opinion", and a
	// mistyped 0 in the settings must not be read as "use the default" — it must be read as the
	// smallest survivable number, so the mistake is visible instead of invisible.
	return FMath::Max(1.f, UTraceSettings::Get().LilyMaxHealth);
}

float UTraceAbilitySetLily::GetWallJumpMomentumScale() const
{
	return 1.f + FMath::Clamp(UTraceSettings::Get().LilyWallJumpMomentumBonus, 0.f, 4.f);
}

// =================================================================================================
// ACTIVATED — ZIP
// =================================================================================================

float UTraceAbilitySetLily::GetActivatedCooldownSeconds() const
{
	return FMath::Max(0.f, UTraceSettings::Get().LilyZipCooldownSeconds);
}

float UTraceAbilitySetLily::GetZipDurationForNow() const
{
	const UTraceSettings& Settings = UTraceSettings::Get();
	float Duration = FMath::Max(0.25f, Settings.LilyZipDurationSeconds);

	// §3's FIRST Core clause: "with the core the duration is halved".
	const ATraceCharacter* MyPawn = GetCharacter();
	const bool bCarrying = (MyPawn != nullptr) && UTraceAbilityComponent::IsCarrier(MyPawn);
	if (bCarrying && CVarLilyZipCoreHalving.GetValueOnAnyThread() != 0)
	{
		Duration *= FMath::Clamp(Settings.LilyZipCarrierDurationScale, 0.05f, 1.f);
	}

	return Duration;
}

bool UTraceAbilitySetLily::CanActivate(FText& OutReason) const
{
	const ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive())
	{
		OutReason = LOCTEXT("LilyNoPawn", "NOT READY");
		return false;
	}

	// Deliberately NO posture condition. §3 gives Zip none — no "must be airborne", no "must be
	// grounded" — so it is castable from anywhere, including off a wall and mid-fall, and that is
	// what makes it an escape as well as a repositioning tool.
	if (bZipping)
	{
		// Re-casting mid-flight would refresh the duration for free, which is not what a 30 s
		// cooldown ability is. Refused rather than refreshed.
		OutReason = LOCTEXT("LilyAlreadyZipping", "ALREADY FLYING");
		return false;
	}

	return true;
}

bool UTraceAbilitySetLily::ActivateAbility()
{
	const ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive() || bZipping)
	{
		return false;
	}

	if (CVarLilyZip.GetValueOnAnyThread() == 0)
	{
		return true;   // RED ARM: the cooldown is still charged, she simply does not fly.
	}

	StartZip();

	// TRUE on the server AND on the owning client. Unlike Quake, everything Zip does is to HER OWN
	// velocity, so predicting it locally is right and is what makes the first half-second of flight
	// feel like the key press rather than like the round trip.
	return true;
}

void UTraceAbilitySetLily::StartZip()
{
	const float Duration = GetZipDurationForNow();

	bZipping = true;
	ZipEndMatchTime = MatchTimeNow() + Duration;
	ZipCorePickups = 0;

	// Re-seeded at the cast, not trusted from the last one: if she is already carrying, the duration
	// above has ALREADY been halved for it, and leaving bWasCarrier false here would halve it a
	// second time on the very next tick.
	const ATraceCharacter* MyPawn = GetCharacter();
	bWasCarrier = (MyPawn != nullptr) && UTraceAbilityComponent::IsCarrier(MyPawn);

	if (HasAuthority())
	{
		PublishState();

		// §5.1's LilyZip row — WORLD side, so this one call multicasts to every machine including
		// this one. It sits on the AUTHORITY side of StartZip on purpose: StartZip also runs on the
		// owning client (Zip is predicted), and although TraceAudio::Play would refuse there anyway,
		// a sound line inside a predicted function is a trap for whoever moves it next.
		//
		// Deliberately NOT PlayPredictedLocal: §1.6 spends prediction on the gunshot and the melee
		// swing only — the two sounds whose round trip a player consciously feels — and a 30-second
		// ability is not in that class.
		TraceAudio::Play(MyPawn, TraceSoundEvents::LilyZip);

		UE_LOG(LogTraceGame, Log,
			TEXT("[Lily] ZIP for %.2fs (carrying=%d at the cast). Jump climbs, crouch descends; the Core "
			     "arriving mid-flight will halve whatever is left."),
			Duration, bWasCarrier ? 1 : 0);
	}
}

void UTraceAbilitySetLily::StopZip(const TCHAR* Why)
{
	if (!bZipping)
	{
		return;
	}

	bZipping = false;
	ZipEndMatchTime = 0.f;
	bJumpHeld = false;

	// The climb level goes with the flight. It would go stale on its own inside a quarter second, but
	// "the ability ended" is a better reason for it to be false than "nobody refreshed it".
	if (UTraceCharacterMovementComponent* MoveComp = GetMovement())
	{
		MoveComp->SetJumpHeld(false);
	}

	// NOTHING HERE TOUCHES THE COOLDOWN. It was started by the framework when ActivateAbility
	// returned true and it keeps running through the flight, through death and through a respawn —
	// spec §5, and spec v19 §4.2 restates it.
	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Verbose, TEXT("[Lily] Zip ended (%s)."), Why);
	}
}

void UTraceAbilitySetLily::HalveRemainingForCorePickup()
{
	if (!bZipping || CVarLilyZipCoreHalving.GetValueOnAnyThread() == 0)
	{
		return;
	}

	const float Now = MatchTimeNow();
	const float Remaining = FMath::Max(0.f, ZipEndMatchTime - Now);
	const float Scale = FMath::Clamp(UTraceSettings::Get().LilyZipCarrierDurationScale, 0.05f, 1.f);

	// *** §3's SECOND CLAUSE, AND THE WHOLE POINT OF THE FUNCTION. ***
	// "If she activates it and then picks up the core, the remaining duration is halved."
	//
	// HALVE WHAT IS LEFT. Do NOT clamp to the carrier duration. With 1 s left this leaves 0.5 s; the
	// natural mistake — min(Remaining, FullDuration x Scale) — would leave 1 s, and the other natural
	// mistake — "restart at the carrier duration" — would leave 2.5 s, i.e. it would LENGTHEN a Zip
	// that was nearly over and reward picking the Core up late. Trace.Lily.ZipVerify stages exactly
	// this case because 2.5 is what the wrong code produces.
	ZipEndMatchTime = Now + (Remaining * Scale);
	++ZipCorePickups;

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Log,
			TEXT("[Lily] Core picked up %d time(s) mid-Zip: %.2fs left became %.2fs (halved what was LEFT, "
			     "not re-clamped to %.2fs)."),
			ZipCorePickups, Remaining, Remaining * Scale,
			FMath::Max(0.25f, UTraceSettings::Get().LilyZipDurationSeconds) * Scale);
	}
}

void UTraceAbilitySetLily::TickAbilities(float DeltaSeconds)
{
	const ATraceCharacter* MyPawn = GetCharacter();

	// --- THE CARRIER EDGE, POLLED ----------------------------------------------------------------
	//
	// Polled rather than pushed because there is no "you became the carrier" hook on the ability set,
	// and adding one would be an edit to a header this pass does not own. 20 Hz is plenty: the worst
	// error is that the halving lands up to 50 ms late, which changes the remaining time by half of
	// whatever was consumed in those 50 ms.
	//
	// AN EDGE, NOT A LEVEL. Halving on "is carrying" would halve every tick and end the Zip inside a
	// quarter of a second.
	const bool bCarrierNow = (MyPawn != nullptr) && MyPawn->IsAlive() && UTraceAbilityComponent::IsCarrier(MyPawn);
	if (bCarrierNow && !bWasCarrier)
	{
		HalveRemainingForCorePickup();
	}
	bWasCarrier = bCarrierNow;

	if (bZipping)
	{
		// BEFORE ApplyZip, never after: this is where the release the framework never delivers is
		// discovered, and reading the level a tick before refreshing it would put a 50 ms delay on
		// every press and every let-go instead of only on the let-go.
		SampleClimbIntent();

		ApplyZip(DeltaSeconds);
	}

	// FX_AUDIO_PLAN §2.1. LAST, AND OUTSIDE THE bZipping TEST — on purpose, twice over:
	//   - bZipping is the LOCAL flight state and is false on every machine but the two that drive
	//     her, so gating the presentation on it would leave every spectator with nothing to see,
	//     which is the exact F10 blocker §1 exists to close. The FX read the replicated flag.
	//   - the cast flash and the end dissolve both OUTLIVE the flight they belong to, so a tick that
	//     only ran while flying could never finish either of them.
	TickZipFx(DeltaSeconds);
}

void UTraceAbilitySetLily::SampleClimbIntent()
{
	// See the header for the missing release hook this exists to work around.
	if (CVarLilyZipHoldRelease.GetValueOnAnyThread() == 0)
	{
		return;   // RED ARM: the latch is left in charge, which is the shipped v21 bug.
	}

	// ONLY THE MACHINE WITH THE KEYBOARD SAMPLES. On the authority for a REMOTE client this returns
	// early and the level arrives instead on FLAG_Custom_1, inside that client's next saved move —
	// which is the point of putting it there.
	if (!IsLocallyControlled())
	{
		return;
	}

	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();
	if (MyPawn == nullptr || MoveComp == nullptr)
	{
		return;
	}

	// A BOT HAS NO KEYBOARD, so there is nothing to sample and nothing to clear. ATraceBotController
	// calls HandleJumpPressed() like a player does; what stops a bot Lily climbing forever is the
	// staleness watchdog inside SetJumpHeld(), which is exactly why that watchdog is there.
	const APlayerController* PC = Cast<APlayerController>(MyPawn->GetController());
	if (PC == nullptr)
	{
		return;
	}

	// THE PLAYER'S OWN BIND, from the same table ApplyControlSettings maps from, so a rebound jump is
	// honoured here without this file knowing any defaults. An unbound action has an invalid key,
	// which IsInputKeyDown would answer nonsense for — in that case say nothing and let the press
	// latch plus the watchdog carry it, rather than pinning her to the floor for the whole flight.
	const FKey JumpKey = UTraceUserSettings::Get().GetKey(ETraceInputAction::Jump);
	if (!JumpKey.IsValid())
	{
		return;
	}

	MoveComp->SetJumpHeld(PC->IsInputKeyDown(JumpKey));
}

bool UTraceAbilitySetLily::IsClimbHeld() const
{
	if (CVarLilyZipHoldRelease.GetValueOnAnyThread() == 0)
	{
		return bJumpHeld;   // RED ARM: the latch, which nothing clears until the flight ends.
	}

	const UTraceCharacterMovementComponent* MoveComp = GetMovement();
	return (MoveComp != nullptr) && MoveComp->IsJumpHeld();
}

void UTraceAbilitySetLily::ApplyZip(float DeltaSeconds)
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();

	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		StopZip(TEXT("no pawn"));
		return;
	}

	if (MatchTimeNow() >= ZipEndMatchTime)
	{
		StopZip(TEXT("duration elapsed"));
		return;
	}

	if (!ShouldDriveMovement())
	{
		return;   // A simulated proxy knows she is flying (for cosmetics) and must not move her.
	}

	// THE DASH OWNS THE VELOCITY VECTOR FOR ITS WHOLE WINDOW, exactly as it does for Mace's suspend.
	// "All other movement mechanics apply as usual" is §3's own sentence, and a dash whose Z was
	// being rewritten 20 times a second would not be the usual dash. It is over in 0.18 s.
	if (MoveComp->IsDashing())
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float WalkSpeed = FMath::Max(1.f, Settings.WalkSpeed);

	// §3 said "jump goes up at walking speed"; DEMO 19 ITEM 4 says "half the speed she moves
	// vertically at", so both scales are 0.5 and the climb is half a walk. Still derived from
	// WalkSpeed rather than written as 400, so a retune of the walk carries the flight with it.
	const float ClimbSpeed   = WalkSpeed * FMath::Clamp(Settings.LilyZipClimbSpeedScale, 0.1f, 4.f);
	const float DescendSpeed = WalkSpeed * FMath::Clamp(Settings.LilyZipDescendSpeedScale, 0.1f, 4.f);

	// THE TWO KEYS, BOTH AS LEVELS. IsCrouchHeld() is the movement component's union of this slice's
	// slide flag and the engine's bWantsToCrouch; IsClimbHeld() is its new twin for jump, and adding
	// it is what makes this line's `!bClimbing` capable of being false at all. See the header.
	const bool bClimbing   = IsClimbHeld();
	const bool bDescending = !bClimbing && MoveComp->IsCrouchHeld();

	float CommandedZ = 0.f;
	if (bClimbing)        { CommandedZ =  ClimbSpeed; }
	else if (bDescending) { CommandedZ = -DescendSpeed; }

	// --- THE ONE TICK OF GRAVITY THIS COMMAND HAS TO SURVIVE --------------------------------------
	//
	// This function runs at 20 Hz; PhysFalling integrates gravity every frame in between. Writing
	// Velocity.Z = 0 therefore does not hover, it falls: over one 50 ms lump the velocity ramps from
	// 0 to -g x dt and the pawn sinks 0.5 x g x dt^2, which is ~1.2 uu a tick, ~24.5 uu/s, ~120 uu
	// across a full 5 s Zip. That is a visible slump in something the doc calls flying.
	//
	// So the command is issued with HALF A TICK OF GRAVITY ALREADY ADDED, which is the closed form
	// that makes the MEAN velocity over the tick exactly CommandedZ (velocity ramps from
	// CommandedZ + g.dt/2 down to CommandedZ - g.dt/2, and the average is CommandedZ). It is a
	// discretisation correction, not frame-accurate flight; see the header for why the honest fix is
	// a movement mode in the prediction path.
	const float GravityPerTick = FMath::Abs(MoveComp->GetGravityZ()) * FMath::Max(0.f, DeltaSeconds);
	MoveComp->Velocity.Z = CommandedZ + (GravityPerTick * 0.5f);

	// AND SHE HAS TO BE OFF THE FLOOR FOR ANY OF THAT TO MEAN ANYTHING. PhysWalking discards Z
	// outright, so a Zip taken while stood on the ground would be five seconds of nothing until she
	// walked off an edge. One nudge into MOVE_Falling on the frame she asks to climb is the whole fix,
	// and it is done through LaunchCharacter rather than SetMovementMode so the engine's own
	// take-off path (and its replication of the mode) runs exactly as it does for a jump.
	if (bClimbing && MoveComp->IsMovingOnGround())
	{
		MyPawn->LaunchCharacter(FVector(0.f, 0.f, ClimbSpeed), /*bXYOverride*/ false, /*bZOverride*/ true);
	}
}

bool UTraceAbilitySetLily::OnJumpPressed()
{
	if (!IsZipping())
	{
		return false;   // Not flying: an ordinary jump, an ordinary wall jump, an ordinary slide-jump.
	}

	bJumpHeld = true;   // the red arm's latch, and nothing else reads it while the fix is on

	// THE PRESS DOES NOT WAIT FOR THE 20 Hz POLL. Setting the level here is what keeps the key
	// feeling instant; SampleClimbIntent's job is the RELEASE, which is the edge nothing else in the
	// project delivers.
	if (UTraceCharacterMovementComponent* MoveComp = GetMovement())
	{
		MoveComp->SetJumpHeld(true);
	}

	// TRUE = CONSUMED, so ACharacter::Jump never runs. §3 rebinds the key for the duration of the
	// flight ("jump goes up at walking speed"), and letting the normal jump fire underneath would add
	// JumpZVelocity on top of the climb and let her ladder off every wall she passed.
	return true;
}

void UTraceAbilitySetLily::OnJumpReleased()
{
	// *** THIS FUNCTION IS DEAD CODE IN A REAL MATCH AND THAT IS THE BUG, NOT AN OVERSIGHT HERE. ***
	// UTraceAbilityComponent::HandleJumpReleased() — its only possible caller — is itself called by
	// nothing; ATracePlayerController::OnJumpCompleted() stops at ACharacter::StopJumping(). It is
	// kept correct and kept wired so that the day somebody adds that one line, the release arrives
	// twice and means the same thing both times. What actually ends the climb today is
	// SampleClimbIntent().
	bJumpHeld = false;

	if (UTraceCharacterMovementComponent* MoveComp = GetMovement())
	{
		MoveComp->SetJumpHeld(false);
	}
}

// =================================================================================================
// Readouts
// =================================================================================================

bool UTraceAbilitySetLily::IsZipping() const
{
	// THE REPLICATED ANSWER, so a HUD, a bot or a spectator on any machine agrees. The local bZipping
	// is what DRIVES her on the two machines entitled to move her; this is what everybody READS.
	return (State().Flags & TraceLilyFlags::Zipping) != 0
		&& State().EffectEndMatchTime > MatchTimeNow();
}

float UTraceAbilitySetLily::GetZipRemaining() const
{
	// The local deadline on the machines that own the flight, the replicated one everywhere else.
	// They agree to within a replication interval, and only the first is allowed to move her.
	const float End = ShouldDriveMovement() && bZipping ? ZipEndMatchTime : State().EffectEndMatchTime;
	if ((!bZipping && !IsZipping()) || End <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(0.f, End - MatchTimeNow());
}

void UTraceAbilitySetLily::PublishState()
{
	if (!HasAuthority())
	{
		return;
	}

	FTraceAbilityNetState& NetState = MutableState();
	if (bZipping)
	{
		NetState.Flags |= TraceLilyFlags::Zipping;
		NetState.EffectEndMatchTime = ZipEndMatchTime;
	}
	else
	{
		NetState.Flags &= static_cast<uint8>(~TraceLilyFlags::Zipping);
		NetState.EffectEndMatchTime = 0.f;
	}

	MarkStateDirty();
}

#if !UE_BUILD_SHIPPING
void UTraceAbilitySetLily::DebugSetZipRemaining(float Seconds)
{
	if (!bZipping)
	{
		return;
	}
	ZipEndMatchTime = MatchTimeNow() + FMath::Max(0.f, Seconds);
	PublishState();
}
#endif

// =================================================================================================
// FX_AUDIO_PLAN §2.1 — THE PRESENTATION
//
// FIVE ELEMENTS, ONE PRODUCER. The replicated Zipping flag's rising edge builds the cast flash, the
// aura and the loop sound; its falling edge dissolves them; the tick drives the wash, the climb
// jet's visibility test and both timelines. Nothing in the input path draws anything, so no machine
// can see two of anything and no machine can see none of it.
//
// WHERE EACH ELEMENT RUNS:
//   cast flash    every machine, on the rising edge          (not on a SYNC — see SyncClientFx)
//   flight aura   every machine, while the flag is up
//   climb jet     every machine, gated on the pawn's own replicated velocity
//   end dissolve  every machine, on the falling edge — EXCEPT death, which is immediate
//   LilyZip       the SERVER, once, World-side: TraceAudio::Play multicasts it to everyone
//   LilyZipLoop   every machine, its own local copy, started and stopped by this router (§1.6.4)
// =================================================================================================

bool UTraceAbilitySetLily::IsZipPresented() const
{
	// THE FLAG, AND ONLY THE FLAG — deliberately not IsZipping(), which also tests the deadline.
	//
	// The FX must switch on exactly the two events the router reports, and the router reports state
	// CHANGES. A visual that also expired on EffectEndMatchTime would fade itself out up to one
	// replication interval before the flag actually fell, and the falling edge would then arrive to
	// find nothing to dissolve — a pop-out dressed up as a fade, which is the one thing bible §6.4
	// forbids by name.
	return (State().Flags & TraceLilyFlags::Zipping) != 0;
}

void UTraceAbilitySetLily::OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New)
{
	const bool bWasFlying = (Old.Flags & TraceLilyFlags::Zipping) != 0;
	const bool bNowFlying = (New.Flags & TraceLilyFlags::Zipping) != 0;

	if (bWasFlying == bNowFlying)
	{
		// A re-arm rather than an on/off: the mid-Zip Core pickup halves EffectEndMatchTime and that
		// is a state change with no visual of its own. §2.1 gives the halving no beat — the HUD chip's
		// countdown is where a player reads it (§7.3) — so this is a deliberate nothing, written down
		// so the next reader does not conclude the edge was forgotten.
		return;
	}

	if (bNowFlying)
	{
		SpawnCastFlash();
		AttachZipFx();
		return;
	}

	// §2.1's end dissolve. NOT immediate: the aura fades I -> 0 over 0.3 s.
	DetachZipFx(/*bImmediate=*/false);
}

void UTraceAbilitySetLily::SyncClientFx(const FTraceAbilityNetState& Current)
{
	// FIRST SIGHT. Idempotent by contract, and the contract is real: this runs again on every
	// character rebuild that lands on Lily, and a second set of rings would be a second set forever.
	if ((Current.Flags & TraceLilyFlags::Zipping) != 0)
	{
		// NO CAST FLASH. A flash is the report of an EVENT, and a machine that has just been told
		// "she is already flying" missed the event by definition — a client joining four seconds into
		// a Zip must inherit the aura, not watch a cast that happened before it connected.
		AttachZipFx();
		return;
	}

	DetachZipFx(/*bImmediate=*/true);
}

ATraceCharacter* UTraceAbilitySetLily::ResolveFxPawn()
{
	ATraceCharacter* Pawn = GetCharacter();

	// §1.2 OBLIGATION 1, and it is not a formality. The set is on the PlayerState and the PlayerState
	// outlives pawns, so "the pawn my components are on" and "the pawn this player owns" are two
	// different questions on any frame that contains a respawn. Answering the second with the first
	// leaves an aura drifting on a body nobody is driving.
	if (FxPawn.Get() != Pawn)
	{
		DetachZipFx(/*bImmediate=*/true);
		FxPawn = Pawn;
	}

	return Pawn;
}

UStaticMeshComponent* UTraceAbilitySetLily::MakeFxPiece(ATraceCharacter* Pawn, const TCHAR* NameHint,
	UStaticMesh* Mesh, TObjectPtr<UMaterialInstanceDynamic>& OutMID, ETraceFxBlend& OutBlend)
{
	OutMID = nullptr;
	OutBlend = ETraceFxBlend::None;

	if (Pawn == nullptr || Mesh == nullptr || Pawn->GetRootComponent() == nullptr)
	{
		return nullptr;
	}

	// OUTERED TO THE PAWN, not to this ability set. RegisterComponent() resolves its owner by casting
	// the outer to an AActor, and a UObject ability set is not one — a piece outered here would never
	// register, never render and never be reaped. The pawn as outer also means a destroyed pawn takes
	// its aura with it, which is §1.2 obligation 2 enforced by the engine rather than by this file.
	UStaticMeshComponent* Piece = NewObject<UStaticMeshComponent>(
		Pawn, MakeUniqueObjectName(Pawn, UStaticMeshComponent::StaticClass(), FName(NameHint)));
	if (Piece == nullptr)
	{
		return nullptr;
	}

	Piece->SetMobility(EComponentMobility::Movable);
	Piece->SetupAttachment(Pawn->GetRootComponent());
	Piece->SetStaticMesh(Mesh);

	// The shared "this is decoration" pass. A COLLIDING piece of FX inside the capsule would break
	// hitscan for the whole flight, which is the one failure TraceFxShapes' header says a verifier
	// checks for.
	UTraceFxShapes::ConfigureFxComponent(Piece);
	Piece->SetCanEverAffectNavigation(false);

	// Starts hidden: the first tick is what places it and lights it, and a ring drawn at its default
	// 100 uu scale for one frame is a white saucer nobody asked for.
	Piece->SetVisibility(false);
	Piece->RegisterComponent();

	// ADDITIVE OR NOTHING (§1.4: "while-active FX are additive only"). Translucent is the request;
	// this project has no translucent parent, so the library resolves it to Additive and the opacity
	// rides as a weight on the colour. Emissive would be WRONG here and not merely different: it is
	// OPAQUE, so a 46 uu ring around her hips would write depth and punch a hole in the arena behind
	// her, and a ring faded to zero would be a dark matte disc rather than gone.
	UMaterialInstanceDynamic* MID = UTraceFxShapes::MakeGlowMID(Piece, 0, ETraceFxBlend::Translucent, OutBlend);

	const bool bUsable = (MID != nullptr)
		&& (OutBlend == ETraceFxBlend::Additive || OutBlend == ETraceFxBlend::Translucent);

	if (!bUsable)
	{
		// NO GREY. The component is KEPT rather than destroyed, so DebugFxPrimitiveCount() reports
		// what was actually created and a "None" run is legible as degradation rather than as an
		// effect that silently did not build. It stays invisible for its whole life.
		OutBlend = ETraceFxBlend::None;
		Piece->SetVisibility(false);
		return Piece;
	}

	OutMID = MID;
	return Piece;
}

UInstancedStaticMeshComponent* UTraceAbilitySetLily::MakeFxRing(ATraceCharacter* Pawn, const TCHAR* NameHint,
	TObjectPtr<UMaterialInstanceDynamic>& OutMID, ETraceFxBlend& OutBlend)
{
	OutMID = nullptr;
	OutBlend = ETraceFxBlend::None;

	UStaticMesh* const Bead = UTraceFxShapes::GetSphere();
	if (Pawn == nullptr || Bead == nullptr || Pawn->GetRootComponent() == nullptr)
	{
		return nullptr;
	}

	UInstancedStaticMeshComponent* Ring = NewObject<UInstancedStaticMeshComponent>(
		Pawn, MakeUniqueObjectName(Pawn, UInstancedStaticMeshComponent::StaticClass(), FName(NameHint)));
	if (Ring == nullptr)
	{
		return nullptr;
	}

	Ring->SetMobility(EComponentMobility::Movable);
	Ring->SetupAttachment(Pawn->GetRootComponent());

	// THE MESH BEFORE ANYTHING ELSE. A UInstancedStaticMeshComponent accepts every AddInstance
	// without one and reports them all back, but a component with no mesh creates no scene proxy and
	// is never handed to the renderer — which is the shipped "Elle's portal is invisible" bug
	// (TraceElleGate.cpp:55-77) and the reason ATraceFxBurst sets the mesh first too.
	Ring->SetStaticMesh(Bead);

	UTraceFxShapes::ConfigureFxComponent(Ring);
	Ring->SetCanEverAffectNavigation(false);
	Ring->SetVisibility(false);
	Ring->RegisterComponent();

	// The instances are added ONCE, at zero scale, and only ever MOVED afterwards. Clearing and
	// re-adding twenty-four instances at 60 Hz for five seconds would rebuild the instance buffer
	// three hundred times for geometry that never changes.
	const FTransform Hidden(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector);
	for (int32 Index = 0; Index < TraceLilyFxFile::BeadsPerRing; ++Index)
	{
		Ring->AddInstance(Hidden);
	}

	// EMISSIVE, NOT ADDITIVE — see EmissiveHueHeadroom above for the two measured reasons. Fallback
	// is accepted as well: M_TraceNeon missing means the beads keep their hue and stop glowing, which
	// is a degradation, and None means nothing resolved at all, which is a hidden ring.
	UMaterialInstanceDynamic* MID = UTraceFxShapes::MakeGlowMID(Ring, 0, ETraceFxBlend::Emissive, OutBlend);

	const bool bUsable = (MID != nullptr) && (OutBlend != ETraceFxBlend::None);

	if (!bUsable)
	{
		OutBlend = ETraceFxBlend::None;
		Ring->SetVisibility(false);
		return Ring;
	}

	OutMID = MID;
	return Ring;
}

void UTraceAbilitySetLily::LayOutRing(UInstancedStaticMeshComponent* Ring, float RadiusUU, float LocalZ,
	float FadeAlpha) const
{
	if (Ring == nullptr)
	{
		return;
	}

	// *** THE RINGS FADE BY SHRINKING, NOT BY DIMMING, AND THAT IS FORCED BY THE BLEND. ***
	// M_TraceNeon is OPAQUE (Scripts/generate_content.py: "neon geometry has to write depth so it
	// occludes and sorts like solid matter"), so a bead dimmed to zero is not gone — it is a black
	// sphere sitting on her hip. FxBurst's header states the same trap for its own faded pieces.
	// Scaling to zero is the only fade an opaque piece has, and it reads correctly here besides: the
	// wash's beads GROW IN at the chest and SHRINK OUT at the feet instead of blinking.
	//
	// The bible §3.4 floor is on the STEADY-STATE bead (12 uu across at r 46). A bead mid-fade is
	// deliberately on its way to invisible, which is not the "thin emissive that dissolves into
	// dashes" the rule is about.
	const float SafeFade = FMath::Clamp(FadeAlpha, 0.f, 1.f);
	const float BeadRadiusUU = TraceLilyFxFile::BeadRadiusFor(RadiusUU) * SafeFade;
	const FVector BeadScale(UTraceFxShapes::ShapeScaleForRadiusUU(BeadRadiusUU));
	const int32 Count = FMath::Min(Ring->GetInstanceCount(), TraceLilyFxFile::BeadsPerRing);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Angle = (2.f * PI * Index) / static_cast<float>(TraceLilyFxFile::BeadsPerRing);
		const FVector Where(RadiusUU * FMath::Cos(Angle), RadiusUU * FMath::Sin(Angle), LocalZ);

		// bMarkRenderStateDirty false in the loop, once at the end: marking it per instance rebuilds
		// the whole buffer twenty-four times for one ring, every frame.
		Ring->UpdateInstanceTransform(Index, FTransform(FQuat::Identity, Where, BeadScale),
			/*bWorldSpace=*/false, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
	}

	Ring->MarkRenderStateDirty();
}

void UTraceAbilitySetLily::AttachZipFx()
{
	ATraceCharacter* Pawn = ResolveFxPawn();
	if (Pawn == nullptr || !Pawn->IsAlive())
	{
		DetachZipFx(/*bImmediate=*/true);
		return;
	}

	// A dissolve that was running is CANCELLED, not raced: she can be re-Zipped by a harness (or by
	// a state re-arm) inside the 0.3 s fade, and leaving the deadline set would dissolve the aura
	// that was just rebuilt.
	DissolveStartTime = 0.f;
	bFxFlying = true;

	if (AuraRingA == nullptr && AuraRingB == nullptr && ClimbJet == nullptr)
	{
		// A FRESH build. The phase restarts here and nowhere else, so an idempotent second call
		// cannot jerk the wash back to the top of its travel.
		AuraPhase = 0.f;

		AuraRingA = MakeFxRing(Pawn, TEXT("LilyZipAuraA"), AuraMIDA, AuraBlendA);
		AuraRingB = MakeFxRing(Pawn, TEXT("LilyZipAuraB"), AuraMIDB, AuraBlendB);
		ClimbJet  = MakeFxPiece(Pawn, TEXT("LilyZipJet"), UTraceFxShapes::GetCone(),
			ClimbJetMID, ClimbJetBlend);

		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Lily] Zip FX attached to %s: aura %s/%s, jet %s (%d of %d primitives)."),
			*GetNameSafe(Pawn),
			UTraceFxShapes::BlendName(AuraBlendA), UTraceFxShapes::BlendName(AuraBlendB),
			UTraceFxShapes::BlendName(ClimbJetBlend),
			(AuraRingA != nullptr) + (AuraRingB != nullptr) + (ClimbJet != nullptr),
			TraceLilyFxFile::MaxAttachedPrimitives);
	}

	if (ZipLoopSound == nullptr)
	{
		// §1.6.4 / §5.1's LilyZipLoop row. LOCAL, no RPC: this router runs on every machine off the
		// same replicated edge, so each machine starts its own copy and the enemy-audible hum the
		// audit asked for costs no bandwidth at all.
		ZipLoopSound = TraceAudio::StartLoopOn(Pawn->GetRootComponent(), TraceSoundEvents::LilyZipLoop,
			TraceLilyFxFile::LoopFadeInSeconds);
	}
}

void UTraceAbilitySetLily::DetachZipFx(bool bImmediate)
{
	bFxFlying = false;

	const bool bHaveAura = (AuraRingA != nullptr) || (AuraRingB != nullptr) || (ClimbJet != nullptr);

	if (!bImmediate && bHaveAura)
	{
		// §2.1's END DISSOLVE. The pieces stay; TickZipFx fades them to zero over 0.3 s and reaps
		// them at the end. Started once — a second falling edge inside the fade must not restart it.
		if (DissolveStartTime <= 0.f)
		{
			DissolveStartTime = MatchTimeNow();
		}

		// The climb jet is NOT in the fade. It is a tell for "she is going up right now", and she is
		// not: it goes out on the same frame the flight does. The tick's visibility test would do
		// this a frame later anyway; doing it here means there is no frame where a jet burns under a
		// pawn that is falling.
		if (ClimbJet != nullptr)
		{
			ClimbJet->SetVisibility(false);
		}

		if (ZipLoopSound != nullptr)
		{
			// bAutoDestroy was FALSE at spawn (TraceAudio.cpp: the caller owns it, so a fade cannot
			// dangle the caller's pointer). Setting it true HERE, on the way out, is what makes the
			// engine reap the component when the ramp finishes — the pointer is dropped in the same
			// breath, so nothing here is holding it any more.
			ZipLoopSound->bAutoDestroy = true;
			ZipLoopSound->FadeOut(TraceLilyFxFile::LoopFadeOutSeconds, 0.f);
			ZipLoopSound = nullptr;
		}
		return;
	}

	// --- IMMEDIATE: death, character swap, a lost pawn --------------------------------------------
	//
	// §1.2 obligation 2: "attached FX must never survive onto a corpse". A 0.3 s fade on a body that
	// has just died is a third of a second of an aura on a corpse, so death does not get the fade.
	TraceLilyFxFile::KillPiece(AuraRingA, AuraMIDA, AuraBlendA);
	TraceLilyFxFile::KillPiece(AuraRingB, AuraMIDB, AuraBlendB);
	TraceLilyFxFile::KillPiece(ClimbJet, ClimbJetMID, ClimbJetBlend);
	TraceLilyFxFile::KillPiece(CastRing, CastRingMID, CastRingBlend);

	if (ZipLoopSound != nullptr)
	{
		ZipLoopSound->Stop();
		ZipLoopSound->DestroyComponent();
		ZipLoopSound = nullptr;
	}

	DissolveStartTime = 0.f;
	CastRingStartTime = 0.f;
	AuraPhase = 0.f;
}

void UTraceAbilitySetLily::SpawnCastFlash()
{
	ATraceCharacter* Pawn = ResolveFxPawn();
	if (Pawn == nullptr || !Pawn->IsAlive())
	{
		return;
	}

	// One flash per cast. A ring left over from a previous one is destroyed rather than reused: its
	// clock would have to be rewound anyway, and two rings would be a fifth primitive.
	TraceLilyFxFile::KillPiece(CastRing, CastRingMID, CastRingBlend);

	CastRing = MakeFxRing(Pawn, TEXT("LilyZipCastRing"), CastRingMID, CastRingBlend);
	CastRingStartTime = MatchTimeNow();

	// Placed at the FEET and left there in the pawn's frame — the ring rides with her rather than
	// staying pinned to the ground. That is the honest read of "cast flash": it says SHE did this,
	// which is what a spectator needs, and Zip is castable in mid-air where there is no ground to
	// pin it to at all.
	if (CastRing != nullptr)
	{
		CastRing->SetRelativeLocation(FVector::ZeroVector);
		CastRing->SetRelativeRotation(FRotator::ZeroRotator);
		LayOutRing(CastRing, TraceLilyFxFile::CastRingStartRadiusUU,
			-TraceLilyFxFile::HalfHeightOf(Pawn) + 2.f, 1.f);
	}
}

void UTraceAbilitySetLily::TickZipFx(float DeltaSeconds)
{
	// The FX tick runs on EVERY machine and on every frame of the component's 20 Hz beat, flying or
	// not: the dissolve and the cast flash both outlive the flag that started them.
	const ATraceCharacter* Pawn = FxPawn.Get();
	ATraceCharacter* const CurrentPawn = GetCharacter();

	if (Pawn == nullptr || Pawn != CurrentPawn)
	{
		// The pawn changed or went away while something was attached. ResolveFxPawn's own detach
		// covers the build path; this covers the drift path, so nothing survives a swap unattended.
		if (AuraRingA != nullptr || AuraRingB != nullptr || ClimbJet != nullptr || CastRing != nullptr)
		{
			DetachZipFx(/*bImmediate=*/true);
			FxPawn = CurrentPawn;
		}
		return;
	}

	// *** THE DEATH RULE, ON EVERY MACHINE. ***
	// OnPawnDied() is a SERVER-ONLY hook (ATraceGameMode is its only caller), so on a client it never
	// runs — and the client is precisely where a corpse wearing an aura would be seen. IsAlive()
	// reads the replicated health component and is therefore true on all three machine roles, which
	// makes this the detach that actually holds the §1.2 obligation up.
	if (!Pawn->IsAlive())
	{
		if (AuraRingA != nullptr || AuraRingB != nullptr || ClimbJet != nullptr || CastRing != nullptr
			|| ZipLoopSound != nullptr)
		{
			DetachZipFx(/*bImmediate=*/true);
		}
		return;
	}

	const float HalfHeight = TraceLilyFxFile::HalfHeightOf(Pawn);
	const float ChestZ = HalfHeight * TraceLilyFxFile::ChestFractionOfHalfHeight;
	const float FeetZ = -HalfHeight;

	// --- the cast flash: r 40 -> 150 uu, I 0.5 -> 0, over 0.4 s ------------------------------------
	if (CastRing != nullptr)
	{
		const float Elapsed = MatchTimeNow() - CastRingStartTime;
		if (Elapsed >= TraceLilyFxFile::CastRingSeconds || CastRingStartTime <= 0.f)
		{
			TraceLilyFxFile::KillPiece(CastRing, CastRingMID, CastRingBlend);
		}
		else
		{
			const float Alpha = FMath::Clamp(Elapsed / TraceLilyFxFile::CastRingSeconds, 0.f, 1.f);
			const float RadiusUU = FMath::Lerp(TraceLilyFxFile::CastRingStartRadiusUU,
				TraceLilyFxFile::CastRingEndRadiusUU, Alpha);

			// The ring GROWS and its beads SHRINK, both linear in the same alpha, so it is gone at
			// 0.4 s exactly. §2.1 spells the fade as "I 0.5 -> 0"; on an opaque blend that is a fade
			// to black rather than a fade to nothing, so the alpha rides the bead size instead — see
			// LayOutRing.
			LayOutRing(CastRing, RadiusUU, FeetZ + 2.f, 1.f - Alpha);

			if (CastRingBlend != ETraceFxBlend::None)
			{
				const FLinearColor Ice = TraceLilyFxFile::LilyAccent();
				UTraceFxShapes::SetGlow(CastRingMID, CastRingBlend, Ice,
					TraceLilyFxFile::HeadroomGlowFor(Ice));
				CastRing->SetVisibility(true);
			}
		}
	}

	// --- the aura: two rings, one path, half a period apart ---------------------------------------
	const bool bDissolving = (DissolveStartTime > 0.f);
	float DissolveScale = 1.f;

	if (bDissolving)
	{
		const float Elapsed = MatchTimeNow() - DissolveStartTime;
		if (Elapsed >= TraceLilyFxFile::DissolveSeconds)
		{
			TraceLilyFxFile::KillPiece(AuraRingA, AuraMIDA, AuraBlendA);
			TraceLilyFxFile::KillPiece(AuraRingB, AuraMIDB, AuraBlendB);
			TraceLilyFxFile::KillPiece(ClimbJet, ClimbJetMID, ClimbJetBlend);
			DissolveStartTime = 0.f;
			return;
		}
		DissolveScale = 1.f - FMath::Clamp(Elapsed / TraceLilyFxFile::DissolveSeconds, 0.f, 1.f);
	}

	if (AuraRingA == nullptr && AuraRingB == nullptr && ClimbJet == nullptr)
	{
		return;
	}

	// REAL TIME, not the match clock: the wash is motion, and motion that hitched with the server
	// clock's smoothing would be visible on a client. DeltaSeconds is the component's own beat.
	AuraPhase = FMath::Fmod(AuraPhase + (DeltaSeconds / TraceLilyFxFile::AuraTravelSeconds), 1.f);

	UInstancedStaticMeshComponent* const Rings[2] = { AuraRingA.Get(), AuraRingB.Get() };
	UMaterialInstanceDynamic* const RingMIDs[2] = { AuraMIDA.Get(), AuraMIDB.Get() };
	const ETraceFxBlend RingBlends[2] = { AuraBlendA, AuraBlendB };

	for (int32 Index = 0; Index < 2; ++Index)
	{
		if (Rings[Index] == nullptr)
		{
			continue;
		}

		// 0.5 apart: the wash has a ring at the halfway point of the travel at every instant, which
		// is what stops the rewrap from reading as a gap. See AuraTravelSeconds for why this is not
		// §2.1's literal "chest and hips" pair.
		const float Phase = FMath::Fmod(AuraPhase + (Index * 0.5f), 1.f);
		const float Z = FMath::Lerp(ChestZ, FeetZ, Phase);

		// EACH RING FADES INTO ITS OWN REWRAP as well as with the dissolve. Without the first term a
		// ring pops out of existence at the feet and reappears at the chest, which is the same
		// pop-out bible §6.4 forbids, once per 0.7 s for the whole flight. sin(PI * phase) is 0 at
		// both ends of the travel and 1 in the middle, so the wash is fullest at the waist.
		const float WrapFade = FMath::Sin(Phase * PI);
		LayOutRing(Rings[Index], TraceLilyFxFile::AuraRingRadiusUU, Z, WrapFade * DissolveScale);

		if (RingBlends[Index] == ETraceFxBlend::None)
		{
			continue;
		}

		// The GLOW is constant and the fade rides the bead SIZE (see LayOutRing): on an opaque blend
		// a dimmed bead is a black bead, and twenty-four black beads round her hips is worse than no
		// aura at all.
		const FLinearColor Ice = TraceLilyFxFile::LilyAccent();
		UTraceFxShapes::SetGlow(RingMIDs[Index], RingBlends[Index], Ice,
			TraceLilyFxFile::HeadroomGlowFor(Ice));
		Rings[Index]->SetVisibility(true);
	}

	// --- the climb jet ----------------------------------------------------------------------------
	if (ClimbJet != nullptr)
	{
		// PLACED AND SIZED EVERY TICK, VISIBLE ONLY WHILE CLIMBING — and the split matters for more
		// than tidiness. Sizing it inside the ascent branch left an unclimbed jet sitting at the
		// engine cone's DEFAULT 100 uu, which is invisible and therefore harmless on screen but
		// prints as "r50 h100" in DebugFxReport and reads there as a mis-sized effect. A probe that
		// reports a number nobody wrote is a probe that costs somebody an afternoon.
		//
		// APEX DOWN. The engine cone's apex is up its own local +Z, so the piece is rolled 180
		// degrees in pitch and hung by its own centre — PlaceConeAlongLocalZ is the helper for a cone
		// lying along a BEAM and would fight the flip here.
		ClimbJet->SetRelativeRotation(FRotator(180.f, 0.f, 0.f));
		ClimbJet->SetRelativeLocation(FVector(0.f, 0.f, FeetZ + (TraceLilyFxFile::JetHeightUU * 0.5f)));
		ClimbJet->SetRelativeScale3D(FVector(
			UTraceFxShapes::ShapeScaleForRadiusUU(TraceLilyFxFile::JetBaseRadiusUU),
			UTraceFxShapes::ShapeScaleForRadiusUU(TraceLilyFxFile::JetBaseRadiusUU),
			UTraceFxShapes::ShapeScaleForLengthUU(TraceLilyFxFile::JetHeightUU)));

		// §2.1: "read from the pawn, no extra replication". A pawn's velocity IS replicated for
		// simulated proxies, so this is correct on every machine without a bit of Lily's own.
		const bool bAscending = !bDissolving && bFxFlying
			&& (Pawn->GetVelocity().Z > TraceLilyFxFile::ClimbVelocityThresholdUU);

		if (!bAscending || ClimbJetBlend == ETraceFxBlend::None)
		{
			ClimbJet->SetVisibility(false);
		}
		else
		{
			UTraceFxShapes::SetGlow(ClimbJetMID, ClimbJetBlend, TraceLilyFxFile::LilyAccent(),
				TraceLilyFxFile::JetIntensity * DissolveScale);
			ClimbJet->SetVisibility(true);
		}
	}
}

#if !UE_BUILD_SHIPPING
int32 UTraceAbilitySetLily::DebugFxPrimitiveCount() const
{
	return (AuraRingA != nullptr ? 1 : 0) + (AuraRingB != nullptr ? 1 : 0)
		+ (ClimbJet != nullptr ? 1 : 0) + (CastRing != nullptr ? 1 : 0);
}

FString UTraceAbilitySetLily::DebugFxReport() const
{
	// MEASURED OFF THE LIVE COMPONENTS, never re-derived from the constants above: a verifier that
	// recomputes the radius it expects is checking its own arithmetic, not the thing on screen.
	// That is the inverse-conversion rule TraceFxShapes states for exactly this reason.
	auto DescribeMesh = [](const TCHAR* Name, const UStaticMeshComponent* Piece, ETraceFxBlend Blend) -> FString
	{
		if (Piece == nullptr)
		{
			return FString::Printf(TEXT("%s=- "), Name);
		}
		const FVector Scale = Piece->GetRelativeScale3D();
		return FString::Printf(TEXT("%s=%s r%.0f h%.0f z%.0f%s "), Name,
			UTraceFxShapes::BlendName(Blend),
			UTraceFxShapes::RadiusUUFromShapeScale(Scale.X),
			UTraceFxShapes::LengthUUFromShapeScale(Scale.Z),
			Piece->GetRelativeLocation().Z,
			Piece->IsVisible() ? TEXT("") : TEXT(" (hidden)"));
	};

	// A RING'S NUMBERS LIVE ON ITS BEADS, not on the component — the component sits at the pawn's
	// origin at scale 1 for the whole flight. Reading its transform would print "r50 z0" for every
	// ring at every radius, which is the kind of probe output that looks like a bug in the effect.
	auto DescribeRing = [](const TCHAR* Name, const UInstancedStaticMeshComponent* Ring,
		ETraceFxBlend Blend) -> FString
	{
		if (Ring == nullptr)
		{
			return FString::Printf(TEXT("%s=- "), Name);
		}
		FTransform Bead;
		if (Ring->GetInstanceCount() <= 0 || !Ring->GetInstanceTransform(0, Bead, /*bWorldSpace=*/false))
		{
			return FString::Printf(TEXT("%s=%s <no beads> "), Name, UTraceFxShapes::BlendName(Blend));
		}
		const FVector Where = Bead.GetLocation();
		return FString::Printf(TEXT("%s=%s r%.0f bead%.0f z%.0f n%d%s "), Name,
			UTraceFxShapes::BlendName(Blend),
			FMath::Sqrt((Where.X * Where.X) + (Where.Y * Where.Y)),
			UTraceFxShapes::RadiusUUFromShapeScale(Bead.GetScale3D().X),
			Where.Z, Ring->GetInstanceCount(),
			Ring->IsVisible() ? TEXT("") : TEXT(" (hidden)"));
	};

	return FString::Printf(TEXT("pieces=%d/%d %s%s%s%sloop=%s"),
		DebugFxPrimitiveCount(), TraceLilyFxFile::MaxAttachedPrimitives,
		*DescribeRing(TEXT("auraA"), AuraRingA, AuraBlendA),
		*DescribeRing(TEXT("auraB"), AuraRingB, AuraBlendB),
		*DescribeMesh(TEXT("jet"), ClimbJet, ClimbJetBlend),
		*DescribeRing(TEXT("cast"), CastRing, CastRingBlend),
		(ZipLoopSound != nullptr && ZipLoopSound->IsPlaying()) ? TEXT("playing") : TEXT("off"));
}
#endif

#if !UE_BUILD_SHIPPING

// =================================================================================================
// Trace.Lily.ZipVerify — SPEC v19 §3's TWO CORE CLAUSES, TWO ARMS, RED FIRST
//
// Both clauses are the same 0.5 at two different moments and the second is the one with a plausible
// wrong answer, so the harness stages the case that TELLS THEM APART: a Zip with 1 s left, then a
// Core pickup. Right answer 0.5 s. The "re-clamp to the carrier duration" mistake answers 2.5 s and
// the "clamp to the smaller" mistake answers 1.0 s, so a single assertion separates all three.
//
// SYNCHRONOUS. ATraceCore::TryPickup is an authority-side debug grant that lands the same frame, and
// TickAbilities(0) is the real edge poll, so every reading below comes from the same call stack that
// caused it. Nothing here waits, and nothing here is a claim about code somebody has read.
// =================================================================================================

// NAMED after the file rather than anonymous — this module builds as a unity/jumbo blob and
// "MakePlayerInto..." is exactly the kind of name another character file would also want.
namespace TraceLilyVerifyFile
{
	UTraceAbilitySetLily* MakePlayerIntoLily(UWorld* WorldPtr, FString& OutWhy)
	{
		if (WorldPtr == nullptr)
		{
			OutWhy = TEXT("no world");
			return nullptr;
		}

		if (!UTraceAbilityComponent::AreCharactersEnabled(WorldPtr))
		{
			OutWhy = TEXT("characters are DISABLED in this match (mode A, or the §3 toggle is off) — "
			              "run this in mode B with characters on");
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = Cast<APlayerController>(It->Get());
			if (PC == nullptr || PC->GetPawn() == nullptr)
			{
				continue;
			}
			UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(PC->GetPawn());
			if (Comp == nullptr || Comp->IsBot())
			{
				continue;
			}

			if (Comp->GetCharacterId() != ETraceCharacterId::Lily)
			{
				Comp->ServerSetCharacter(ETraceCharacterId::Lily);
			}

			if (UTraceAbilitySetLily* Found = Comp->GetAbilitySetAs<UTraceAbilitySetLily>())
			{
				return Found;
			}

			OutWhy = FString::Printf(
				TEXT("ServerSetCharacter(Lily) did not produce a UTraceAbilitySetLily (id is now %s) — a "
				     "team-mate may already hold her, or the reflection roster did not find the class"),
				TraceCharacterIdToString(Comp->GetCharacterId()));
			return nullptr;
		}

		OutWhy = TEXT("no human player controller with a pawn");
		return nullptr;
	}

	/**
	 * *** WHY EVERY KEY-PRESSING HARNESS IN THIS FILE NOW WAITS. ***
	 *
	 * The v22 baseline run of Trace.Lily.FlightTest reported climb 0 uu, sag 0 uu, z 90 -> 90 -> 90 in
	 * BOTH arms and a verdict of INVALID. Nothing was wrong with the flight: the CHARACTER SELECT
	 * SCREEN was still open, so the world was PAUSED, and the E press the harness fired to cast Zip was
	 * eaten by the screen as a card pick — the log line right after it reads "[CharSelect] Requesting
	 * SLIMEBALL". The harness then spent both arms measuring a paused Slimeball.
	 *
	 * That is worth a named function rather than a comment: a harness that runs at the wrong moment
	 * does not fail, it produces zeroes, and zeroes are the easiest number in the world to mistake for
	 * "the ability does nothing".
	 *
	 * The select screen pauses the world and the match does not, so IsPaused() is the exact gate.
	 */
	bool IsWorldReadyForKeyPresses(const UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr || WorldPtr->IsPaused())
		{
			return false;
		}

		const APlayerController* PC = WorldPtr->GetFirstPlayerController();
		return (PC != nullptr) && (PC->GetPawn() != nullptr);
	}

	/** The local game world — the one with a keyboard, which is not always the authoritative one. */
	UWorld* FindLocalKeyboardWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() != nullptr && Context.World()->IsGameWorld()
				&& Context.World()->GetFirstPlayerController() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	void RunZipVerify()
	{
		const TCHAR* const Tag = TEXT("LILYZIP");

		UWorld* TestWorld = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && Context.World()->IsGameWorld()
					&& Context.World()->GetAuthGameMode() != nullptr)
				{
					TestWorld = Context.World();
					break;
				}
			}
		}

		if (TestWorld == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — no authoritative game world. Run this on the server/host, in a live match."),
				Tag);
			return;
		}

		FString Why;
		UTraceAbilitySetLily* Lily = MakePlayerIntoLily(TestWorld, Why);
		if (Lily == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — %s."), Tag, *Why);
			return;
		}

		ATraceCharacter* MyPawn = Lily->GetCharacter();
		ATraceCore* CoreActor = ATraceCore::Get(TestWorld);
		if (MyPawn == nullptr || CoreActor == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — Lily has no pawn, or there is no Core in this world."), Tag);
			return;
		}

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Lily.ZipCoreHalving"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — Trace.Lily.ZipCoreHalving is not registered."), Tag);
			return;
		}
		const int32 ArmBefore = Arm->GetInt();

		const UTraceSettings& Settings = UTraceSettings::Get();
		const float FullDuration    = FMath::Max(0.25f, Settings.LilyZipDurationSeconds);
		const float CarrierDuration = FullDuration * FMath::Clamp(Settings.LilyZipCarrierDurationScale, 0.05f, 1.f);

		// The staged case. 1 s left is BELOW the carrier duration (2.5 s), which is the only region
		// where the right answer and the two wrong ones differ.
		constexpr float StagedRemaining = 1.f;

		float RedCastCarrying = -1.f;
		float RedAfterPickup  = -1.f;
		float GreenCastCarrying = -1.f;
		float GreenAfterPickup  = -1.f;

		for (int32 ArmIndex = 0; ArmIndex < 2; ++ArmIndex)
		{
			const bool bGreen = (ArmIndex == 1);
			Arm->Set(bGreen ? 1 : 0, ECVF_SetByConsole);

			// ---- CLAUSE 1: cast WHILE carrying. Expect the duration halved. -----------------------
			CoreActor->TryPickup(MyPawn);
			Lily->OnHalfTime();                      // clears any flight left from the previous arm
			Lily->TickAbilities(0.f);                // settles the carrier edge before the cast
			Lily->ActivateAbility();
			const float CastLength = Lily->GetZipRemaining();

			// ---- CLAUSE 2: cast WITHOUT the Core, spend it down to 1 s, then pick the Core up. ----
			//
			// The Core is taken off her first, so the pickup below is a genuine NOT-carrying ->
			// carrying edge rather than a level the poll would never see.
			Lily->OnHalfTime();
			ATraceCharacter* Parker = nullptr;
			for (TActorIterator<ATraceCharacter> It(TestWorld); It; ++It)
			{
				if (*It != nullptr && *It != MyPawn && (*It)->IsAlive()) { Parker = *It; break; }
			}
			if (Parker != nullptr)
			{
				CoreActor->TryPickup(Parker);
			}
			Lily->TickAbilities(0.f);
			Lily->ActivateAbility();
			Lily->DebugSetZipRemaining(StagedRemaining);
			CoreActor->TryPickup(MyPawn);
			Lily->TickAbilities(0.f);                // the real edge poll, and therefore the real halving
			const float AfterPickup = Lily->GetZipRemaining();

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] arm=%s  cast-while-carrying %.2fs (full %.2fs, halved %.2fs) | "
				     "%.2fs left + Core pickup -> %.2fs (right answer %.2f; 're-clamp' would say %.2f; "
				     "'clamp to smaller' would say %.2f)"),
				Tag, bGreen ? TEXT("GREEN (shipped)") : TEXT("RED (halving removed)"),
				CastLength, FullDuration, CarrierDuration,
				StagedRemaining, AfterPickup, StagedRemaining * 0.5f, CarrierDuration, StagedRemaining);

			if (bGreen) { GreenCastCarrying = CastLength; GreenAfterPickup = AfterPickup; }
			else        { RedCastCarrying   = CastLength; RedAfterPickup   = AfterPickup; }
		}

		Arm->Set(ArmBefore, ECVF_SetByConsole);
		Lily->OnHalfTime();

		// ---- the verdict ---------------------------------------------------------------------------
		//
		// THE RED ARM HAS TO REPRODUCE FIRST. With the halving removed the cast must give the FULL
		// duration and the pickup must leave the staged time untouched; if either of those already
		// looks halved, the arm is not disarming what it claims to and the green readings mean nothing.
		const bool bRedReproduced =
			FMath::IsNearlyEqual(RedCastCarrying, FullDuration, 0.15f)
			&& FMath::IsNearlyEqual(RedAfterPickup, StagedRemaining, 0.15f);

		if (!bRedReproduced)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — the red arm did not reproduce. With the halving removed the cast "
				     "gave %.2fs (expected the full %.2fs) and the pickup left %.2fs (expected the staged %.2fs). "
				     "A harness that cannot go red has proved nothing."),
				Tag, RedCastCarrying, FullDuration, RedAfterPickup, StagedRemaining);
			return;
		}

		int32 Failed = 0;
		if (!FMath::IsNearlyEqual(GreenCastCarrying, CarrierDuration, 0.15f))
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: §3 'with the core the duration is halved' — casting while carrying gave %.2fs, "
				     "expected %.2fs."),
				Tag, GreenCastCarrying, CarrierDuration);
		}
		if (!FMath::IsNearlyEqual(GreenAfterPickup, StagedRemaining * 0.5f, 0.15f))
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: §3 'if she activates it and then picks up the core, the REMAINING duration is "
				     "halved' — %.2fs left became %.2fs, expected %.2fs. (%.2fs would mean it re-clamped to the "
				     "carrier duration and LENGTHENED a nearly-spent Zip.)"),
				Tag, StagedRemaining, GreenAfterPickup, StagedRemaining * 0.5f, CarrierDuration);
		}

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — with the halving removed a carried cast lasted %.2fs and a pickup left "
				     "%.2fs untouched; with it in place the identical calls gave %.2fs and %.2fs. The second is the "
				     "subtle clause: what was LEFT halved, it did not re-clamp to %.2fs."),
				Tag, RedCastCarrying, RedAfterPickup, GreenCastCarrying, GreenAfterPickup, CarrierDuration);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d of 2 clauses wrong."), Tag, Failed);
		}
	}

	FAutoConsoleCommand CmdZipVerify(
		TEXT("Trace.Lily.ZipVerify"),
		TEXT("SPEC v19 §3. Two arms, RED FIRST: proves Zip's two Core clauses — halved at the cast while "
		     "carrying, and halving WHAT IS LEFT on a mid-flight pickup (staged at 1s left, where the right "
		     "answer 0.5s and the natural mistakes 1.0s / 2.5s all differ)."),
		FConsoleCommandDelegate::CreateStatic(&RunZipVerify));

	// =============================================================================================
	// Trace.Lily.FlightTest — DOES SHE ACTUALLY FLY, DRIVEN BY THE REAL KEYS
	//
	// ZipVerify above is arithmetic: it proves the two Core clauses and touches no physics at all. It
	// would report PASS on a build where pressing E did nothing to the pawn, which is EXACTLY the
	// failure Demo 17 item 1 was about — Elle's Snap was reported PROVEN by a harness that called
	// TryActivate() instead of pressing a key. So this one presses E and SPACE through Trace.SimInput,
	// i.e. through the engine's own input pipeline, and judges from the pawn's WORLD Z.
	//
	// TWO ARMS, RED FIRST: Trace.Lily.Zip 0 accepts the press, charges the cooldown and does not fly
	// her, so the climb has to collapse to roughly an ordinary jump. If it does not, the instrument is
	// measuring something other than the ability and says INVALID.
	//
	// IT ALSO MEASURES THE HOVER SAG, which is the one claim this file makes about itself that could
	// quietly be false: ApplyZip adds half a tick of gravity to the commanded Z precisely so that
	// "hold nothing and hang there" does not sink at ~24.5 uu/s. The number below is that claim, in uu.
	// =============================================================================================

	struct FLilyFlightState
	{
		/** 0 = RED (Trace.Lily.Zip 0), 1 = GREEN (shipped). */
		int32 Arm = 0;
		/** -2 waiting for the select screen to close, -1 arm setup, 0 climbing, 1 hovering. */
		int32 Phase = -2;

		/** Real-time deadline for the -2 wait. See IsWorldReadyForKeyPresses. */
		double ReadyGiveUpAt = 0.0;

		/** REAL time. The select screen can pause the world; every other harness here learned that. */
		double PhaseDeadline = 0.0;

		TWeakObjectPtr<UTraceAbilitySetLily> Lily;

		float StartZ = 0.f;
		float ClimbTopZ = 0.f;

		float RedClimb = -1.f;
		float GreenClimb = -1.f;
		float RedSag = 0.f;
		float GreenSag = 0.f;
		bool bRedRan = false;
		bool bGreenRan = false;
		bool bZippedInGreen = false;
	};

	/** Long enough to separate a climb from a jump, short enough to fit inside a 5 s Zip with room. */
	constexpr float FlightClimbSeconds = 1.2f;
	/** The sag watch. 2 s of "hold nothing": uncorrected this would lose ~49 uu, corrected ~0. */
	constexpr float FlightHoverSeconds = 2.f;

	void FinishFlightTest(FLilyFlightState* State)
	{
		const TCHAR* const Tag = TEXT("LILYFLIGHT");
		const UTraceSettings& Settings = UTraceSettings::Get();
		const float ExpectedClimb =
			FMath::Max(1.f, Settings.WalkSpeed) * FMath::Clamp(Settings.LilyZipClimbSpeedScale, 0.1f, 4.f)
			* FlightClimbSeconds;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] RED (Zip disabled) climbed %.0f uu in %.1fs | GREEN (shipped) climbed %.0f uu and then "
			     "drifted %.1f uu over %.1fs of holding nothing. Expected climb ~%.0f uu (%.0f uu/s x %.1fs)."),
			Tag, State->RedClimb, FlightClimbSeconds, State->GreenClimb, State->GreenSag,
			FlightHoverSeconds, ExpectedClimb,
			Settings.WalkSpeed * Settings.LilyZipClimbSpeedScale, FlightClimbSeconds);

		// THE FIXTURE PROVES ITSELF FIRST. With Zip off, holding jump is an ordinary jump: it must not
		// come anywhere near a full climb. If it did, this harness is measuring gravity and a jump arc
		// rather than the ability, and its green reading would mean nothing.
		if (!State->bRedRan || !State->bGreenRan || !State->bZippedInGreen
			|| State->RedClimb > ExpectedClimb * 0.5f)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — redRan=%d greenRan=%d sheWasActuallyFlyingInGreen=%d "
				     "redClimb=%.0f uu (must stay under half of %.0f, or the climb is not the ability). "
				     "A harness that cannot go red has proved nothing."),
				Tag, State->bRedRan ? 1 : 0, State->bGreenRan ? 1 : 0, State->bZippedInGreen ? 1 : 0,
				State->RedClimb, ExpectedClimb);
			return;
		}

		int32 Failed = 0;
		if (State->GreenClimb < ExpectedClimb * 0.6f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: §3 'jump goes up at walking speed' — holding jump for %.1fs raised her %.0f uu, "
				     "expected about %.0f."),
				Tag, FlightClimbSeconds, State->GreenClimb, ExpectedClimb);
		}
		if (FMath::Abs(State->GreenSag) > 60.f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: holding neither key drifted her %.1f uu in %.1fs. ApplyZip's half-tick gravity "
				     "correction is not doing its job; uncorrected this would be about %.0f uu."),
				Tag, State->GreenSag, FlightHoverSeconds, 24.5f * FlightHoverSeconds);
		}

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — the SAME key presses raised her %.0f uu with Zip on and %.0f uu with it "
				     "off, and she held her height to within %.1f uu while flying."),
				Tag, State->GreenClimb, State->RedClimb, FMath::Abs(State->GreenSag));
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d of 2 checks failed."), Tag, Failed);
		}
	}

	void RunFlightTest()
	{
		const TCHAR* const Tag = TEXT("LILYFLIGHT");

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Lily.Zip"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — Trace.Lily.Zip is not registered."), Tag);
			return;
		}

		FLilyFlightState* State = new FLilyFlightState();
		State->ReadyGiveUpAt = FPlatformTime::Seconds() + 30.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] armed. Waiting for a live, UNPAUSED match with a local pawn, then two arms, RED first, "
			     "~%.1fs each, driving the REAL E and SPACE keys."),
			Tag, FlightClimbSeconds + FlightHoverSeconds + 0.5f);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, Arm](float /*Delta*/) -> bool
			{
				const double Now = FPlatformTime::Seconds();

				// --- PHASE -2: WAIT FOR A WORLD THAT WILL ACCEPT A KEY PRESS ---------------------
				//
				// And claim Lily only once it will. Doing it earlier is what produced the v22 baseline's
				// three zeroes: the select screen was still up, it ate the E as a card pick, and the run
				// measured a paused Slimeball. See IsWorldReadyForKeyPresses.
				if (State->Phase == -2)
				{
					UWorld* WaitWorld = FindLocalKeyboardWorld();
					if (!IsWorldReadyForKeyPresses(WaitWorld))
					{
						if (Now > State->ReadyGiveUpAt)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[LILYFLIGHT] VERDICT: INVALID — 30s went by with no unpaused match and a "
								     "local pawn. A key cannot be pressed on a machine nobody is sitting at, and "
								     "the select screen would have eaten it anyway."));
							delete State;
							return false;
						}
						return true;
					}

					FString Why;
					UTraceAbilitySetLily* Claimed = MakePlayerIntoLily(WaitWorld, Why);
					if (Claimed == nullptr || Claimed->GetCharacter() == nullptr)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[LILYFLIGHT] VERDICT: INVALID — %s."),
							(Claimed == nullptr) ? *Why : TEXT("Lily has no pawn"));
						delete State;
						return false;
					}

					State->Lily = Claimed;
					State->Phase = -1;
					return true;
				}

				UTraceAbilitySetLily* TickLily = State->Lily.Get();
				ATraceCharacter* Pawn = (TickLily != nullptr) ? TickLily->GetCharacter() : nullptr;
				UWorld* TickWorld = (Pawn != nullptr) ? Pawn->GetWorld() : nullptr;

				if (TickLily == nullptr || Pawn == nullptr || TickWorld == nullptr || !Pawn->IsAlive())
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[LILYFLIGHT] VERDICT: INVALID — Lily's pawn went away mid-test (died, or the match "
						     "moved on). Run this early in a match."));
					Arm->Set(1, ECVF_SetByConsole);
					delete State;
					return false;
				}

				if (State->Phase == -1)
				{
					Arm->Set(State->Arm, ECVF_SetByConsole);

					// The framework's own cooldown clear, so the SECOND arm's E is not refused by the
					// 30 s the FIRST arm just charged. This is the one function allowed to zero it.
					if (UTraceAbilityComponent* Comp = TickLily->GetAbilityComponent())
					{
						Comp->OnHalfTime();
					}

					State->StartZ = static_cast<float>(Pawn->GetActorLocation().Z);

					// THE REAL KEYS, THROUGH THE REAL PIPELINE. Not TryActivate(), not OnJumpPressed().
					GEngine->Exec(TickWorld, TEXT("Trace.SimInput E 0.05"));
					GEngine->Exec(TickWorld, *FString::Printf(TEXT("Trace.SimInput SpaceBar %.2f"), FlightClimbSeconds));

					State->Phase = 0;
					State->PhaseDeadline = Now + FlightClimbSeconds;
					return true;
				}

				if (State->Phase == 0)
				{
					// Sampled every tick rather than only at the deadline, so "she was flying" is a fact
					// about the window and not about one instant of it.
					if (TickLily->IsZipping() && State->Arm == 1)
					{
						State->bZippedInGreen = true;
					}
					if (Now < State->PhaseDeadline)
					{
						return true;
					}

					State->ClimbTopZ = static_cast<float>(Pawn->GetActorLocation().Z);
					State->Phase = 1;
					State->PhaseDeadline = Now + FlightHoverSeconds;
					return true;
				}

				if (Now < State->PhaseDeadline)
				{
					return true;
				}

				const float EndZ = static_cast<float>(Pawn->GetActorLocation().Z);
				const float Climb = State->ClimbTopZ - State->StartZ;
				const float Sag = EndZ - State->ClimbTopZ;

				if (State->Arm == 0) { State->RedClimb = Climb;   State->RedSag = Sag;   State->bRedRan = true; }
				else                 { State->GreenClimb = Climb; State->GreenSag = Sag; State->bGreenRan = true; }

				UE_LOG(LogTraceGame, Display,
					TEXT("[LILYFLIGHT] arm=%s  climb %.0f uu in %.1fs, then %+.1f uu over %.1fs of no input "
					     "(z %.0f -> %.0f -> %.0f)."),
					(State->Arm == 1) ? TEXT("GREEN (shipped)") : TEXT("RED (Zip disabled)"),
					Climb, FlightClimbSeconds, Sag, FlightHoverSeconds,
					State->StartZ, State->ClimbTopZ, EndZ);

				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Phase = -1;
					return true;
				}

				Arm->Set(1, ECVF_SetByConsole);
				FinishFlightTest(State);
				delete State;
				return false;
			}),
			0.f);
	}

	FAutoConsoleCommand CmdFlightTest(
		TEXT("Trace.Lily.FlightTest"),
		TEXT("SPEC v19 §3. Two arms, RED first: presses the REAL E and SPACE keys through Trace.SimInput and "
		     "measures the pawn's world Z, so 'she can fly' is a measurement and not a claim. Also reports the "
		     "hover drift the 20 Hz gravity correction exists to remove."),
		FConsoleCommandDelegate::CreateStatic(&RunFlightTest));

	// =============================================================================================
	// Trace.Lily.KeyTest — DEMO 19 ITEM 4, FROM THE PLAYER'S CHAIR
	//
	// The complaint is not a number, it is a sequence of key presses with a wrong outcome:
	//
	//     "Pressing space once makes her continuously move up even after I let go,
	//      and pressing control doesn't make her come down."
	//
	// So this harness performs exactly that sequence, on the real keys, through the real input
	// pipeline, and judges it by where the pawn ENDS UP:
	//
	//     E                       cast Zip
	//     hold JUMP    1.0 s      she should rise
	//     LET GO       1.0 s      *** SHE SHOULD STOP RISING ***      <- half the complaint
	//     hold CROUCH  0.8 s      *** SHE SHOULD COME DOWN ***        <- the other half
	//
	// TWO ARMS, RED FIRST, and the red arm is not an imitation: Trace.Lily.ZipHoldRelease 0 selects
	// the shipped v21 code path (the latch that nothing ever cleared), so the RED numbers ARE the
	// user's bug. If red does not reproduce — if letting go already stopped her — then the harness is
	// not measuring its rule and it says INVALID rather than PASS.
	//
	// IT ALSO MEASURES THE SPEED, because "half the speed she moves vertically at" is item 4's third
	// sentence and a knob set to 0.5 is not evidence that anything moves at half speed.
	// =============================================================================================

	struct FLilyKeyTestState
	{
		/** 0 = RED (Trace.Lily.ZipHoldRelease 0 — the shipped bug), 1 = GREEN (the fix). */
		int32 Arm = 0;

		/** -2 wait for an unpaused match, -1 cast and settle, 0 hold jump, 1 let go, 2 hold crouch. */
		int32 Phase = -2;

		double PhaseDeadline = 0.0;
		double ReadyGiveUpAt = 0.0;

		TWeakObjectPtr<UTraceAbilitySetLily> Lily;

		FString JumpKeyName;
		FString CrouchKeyName;

		/** Z at each phase boundary, in world units. */
		float ZAtHoldStart = 0.f;
		float ZAtRelease = 0.f;
		float ZAfterCoast = 0.f;

		/** Per arm [RED, GREEN]: uu climbed while held, uu drifted after letting go, uu moved on crouch. */
		float Climb[2] = { 0.f, 0.f };
		float Coast[2] = { 0.f, 0.f };
		float Crouched[2] = { 0.f, 0.f };
		bool bRan[2] = { false, false };
		bool bFlew[2] = { false, false };
	};

	/**
	 * HOW LONG THE HARNESS WAITS AFTER SWAPPING THE PLAYER TO LILY BEFORE IT PRESSES E — and the
	 * reason it exists at all, found by the v23 integration pass.
	 *
	 * Both key harnesses reported "VERDICT: INVALID — the red arm did not reproduce the complaint",
	 * twice in a row, with flying(red/green)=0/1: arm 1's E was thrown away, so she never took off
	 * at all in the red arm, while arm 2's identical E three seconds later worked every time. That
	 * is not the fix failing and it is not the bug being absent — it is the FIRST press racing the
	 * character swap. MakePlayerIntoLily() rebuilds the ability component, and a press delivered
	 * inside that same tick has nothing to activate.
	 *
	 * It only started showing up here because this pass killed twelve orphaned editors and the
	 * machine got fast enough for one tick to be short. The earlier passes' green results were
	 * bought by a loaded machine, which is not a thing to rely on.
	 *
	 * 0.75 s is several frames at any rate this project runs at and costs the run under a second.
	 */
	constexpr float KeyTestClaimSettleSeconds = 0.75f;

	constexpr float KeyTestSettleSeconds = 0.40f;   // Zip live, and any fall from the previous arm arrested
	constexpr float KeyTestHoldSeconds   = 1.00f;   // jump held
	constexpr float KeyTestCoastSeconds  = 1.00f;   // nothing held — THE COMPLAINT
	constexpr float KeyTestCrouchSeconds = 0.80f;   // crouch held — THE OTHER HALF

	void FinishKeyTest(FLilyKeyTestState* State)
	{
		const TCHAR* const Tag = TEXT("LILYKEYS");
		const UTraceSettings& Settings = UTraceSettings::Get();

		const float WalkSpeed    = FMath::Max(1.f, Settings.WalkSpeed);
		const float ClimbSpeed   = WalkSpeed * FMath::Clamp(Settings.LilyZipClimbSpeedScale, 0.1f, 4.f);
		const float DescendSpeed = WalkSpeed * FMath::Clamp(Settings.LilyZipDescendSpeedScale, 0.1f, 4.f);
		const float ExpectedClimb   = ClimbSpeed * KeyTestHoldSeconds;
		const float ExpectedDescend = DescendSpeed * KeyTestCrouchSeconds;

		for (int32 Index = 0; Index < 2; ++Index)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] arm=%-22s  hold jump %.1fs: %+.0f uu  |  LET GO %.1fs: %+.0f uu  |  "
				     "hold crouch %.1fs: %+.0f uu   (flying=%d)"),
				Tag,
				(Index == 1) ? TEXT("GREEN (the fix)") : TEXT("RED (shipped v21 bug)"),
				KeyTestHoldSeconds, State->Climb[Index],
				KeyTestCoastSeconds, State->Coast[Index],
				KeyTestCrouchSeconds, State->Crouched[Index],
				State->bFlew[Index] ? 1 : 0);
		}

		// ---- THE RED ARM HAS TO REPRODUCE THE USER'S SENTENCE, OR NOTHING BELOW COUNTS -------------
		//
		// Both halves of it. "She kept going up after I let go" is Coast >> 0, and "control did not
		// bring her down" is Crouched >= 0 — under the latch she was still climbing through the crouch,
		// because the descend branch was behind `!climbing`.
		const bool bRedKeptClimbing  = State->Coast[0] > ExpectedClimb * 0.5f;
		const bool bRedIgnoredCrouch = State->Crouched[0] > -ExpectedDescend * 0.1f;

		if (!State->bRan[0] || !State->bRan[1] || !State->bFlew[0] || !State->bFlew[1]
			|| !bRedKeptClimbing || !bRedIgnoredCrouch)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — the red arm did not reproduce the complaint. "
				     "redRan=%d greenRan=%d flying(red/green)=%d/%d | after letting go she moved %+.0f uu "
				     "(the bug needs more than %+.0f) | on crouch she moved %+.0f uu (the bug needs it to be "
				     "no lower than %+.0f). A harness that cannot go red has proved nothing."),
				Tag, State->bRan[0] ? 1 : 0, State->bRan[1] ? 1 : 0,
				State->bFlew[0] ? 1 : 0, State->bFlew[1] ? 1 : 0,
				State->Coast[0], ExpectedClimb * 0.5f,
				State->Crouched[0], -ExpectedDescend * 0.1f);
			return;
		}

		int32 Failed = 0;

		// (1) She still flies at all.
		if (State->Climb[1] < ExpectedClimb * 0.6f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: holding jump for %.1fs raised her %+.0f uu, expected about %.0f "
				     "(%.0f uu/s). The fix must not have cost her the climb."),
				Tag, KeyTestHoldSeconds, State->Climb[1], ExpectedClimb, ClimbSpeed);
		}

		// (2) *** "even after I let go" ***
		if (State->Coast[1] > ExpectedClimb * 0.25f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: 'pressing space once makes her continuously move up even after I let go' — "
				     "she rose another %+.0f uu in the %.1fs after the key came up. The release is still not "
				     "reaching her."),
				Tag, State->Coast[1], KeyTestCoastSeconds);
		}

		// (3) *** "pressing control doesn't make her come down" ***
		if (State->Crouched[1] > -ExpectedDescend * 0.4f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: 'pressing control doesn't make her come down' — %.1fs of crouch moved her "
				     "%+.0f uu, expected about %+.0f (%.0f uu/s down)."),
				Tag, KeyTestCrouchSeconds, State->Crouched[1], -ExpectedDescend, DescendSpeed);
		}

		// (4) *** "half the speed she moves vertically at" ***, MEASURED and not read off the knob.
		const float MeasuredClimbRate = State->Climb[1] / KeyTestHoldSeconds;
		if (MeasuredClimbRate > WalkSpeed * 0.65f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: 'half the speed she moves vertically at' — she climbed at %.0f uu/s against a "
				     "walk of %.0f. Half a walk is %.0f."),
				Tag, MeasuredClimbRate, WalkSpeed, WalkSpeed * 0.5f);
		}

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — the SAME three key presses. WITH THE BUG: let go and she rose "
				     "another %+.0f uu, then crouch moved her %+.0f uu (still upward). FIXED: letting go left "
				     "her within %+.0f uu, and crouch took her DOWN %+.0f uu. She climbs at %.0f uu/s, half "
				     "the %.0f walk."),
				Tag, State->Coast[0], State->Crouched[0], State->Coast[1], State->Crouched[1],
				MeasuredClimbRate, WalkSpeed);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d of 4 checks failed."), Tag, Failed);
		}
	}

	void RunKeyTest()
	{
		const TCHAR* const Tag = TEXT("LILYKEYS");

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Lily.ZipHoldRelease"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — Trace.Lily.ZipHoldRelease is not registered, so there is no red arm."),
				Tag);
			return;
		}

		FLilyKeyTestState* State = new FLilyKeyTestState();
		State->ReadyGiveUpAt = FPlatformTime::Seconds() + 30.0;

		// THE PLAYER'S OWN BINDS, not the literals SpaceBar and LeftControl. If somebody rebinds jump
		// this harness follows them, which is the only way it can claim to be pressing "the key the
		// user pressed" rather than "the key we assume they pressed".
		const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();
		const FKey JumpKey   = UserSettings.GetKey(ETraceInputAction::Jump);
		const FKey CrouchKey = UserSettings.GetKey(ETraceInputAction::Crouch);
		if (!JumpKey.IsValid() || !CrouchKey.IsValid())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — jump or crouch is UNBOUND (jump='%s', crouch='%s'). There is no "
				     "key to press."),
				Tag, *JumpKey.ToString(), *CrouchKey.ToString());
			delete State;
			return;
		}
		State->JumpKeyName   = JumpKey.ToString();
		State->CrouchKeyName = CrouchKey.ToString();

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] armed. Two arms, RED (the shipped bug) first. Per arm: E, hold %s %.1fs, LET GO %.1fs, "
			     "hold %s %.1fs — judged by the pawn's world Z."),
			Tag, *State->JumpKeyName, KeyTestHoldSeconds, KeyTestCoastSeconds,
			*State->CrouchKeyName, KeyTestCrouchSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, Arm](float /*Delta*/) -> bool
			{
				const double Now = FPlatformTime::Seconds();

				if (State->Phase == -2)
				{
					UWorld* WaitWorld = FindLocalKeyboardWorld();
					if (!IsWorldReadyForKeyPresses(WaitWorld))
					{
						if (Now > State->ReadyGiveUpAt)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[LILYKEYS] VERDICT: INVALID — 30s went by with no unpaused match and a local "
								     "pawn. The select screen pauses the world and eats the E."));
							delete State;
							return false;
						}
						return true;
					}

					FString Why;
					UTraceAbilitySetLily* Claimed = MakePlayerIntoLily(WaitWorld, Why);
					if (Claimed == nullptr || Claimed->GetCharacter() == nullptr)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[LILYKEYS] VERDICT: INVALID — %s."),
							(Claimed == nullptr) ? *Why : TEXT("Lily has no pawn"));
						delete State;
						return false;
					}

					State->Lily = Claimed;
					State->Phase = -1;
					// See KeyTestClaimSettleSeconds: arm 1's E used to race the character swap and be
					// dropped, which read out as "the red arm cannot reproduce the bug".
					State->PhaseDeadline = Now + KeyTestClaimSettleSeconds;
					return true;
				}

				UTraceAbilitySetLily* TickLily = State->Lily.Get();
				ATraceCharacter* Pawn = (TickLily != nullptr) ? TickLily->GetCharacter() : nullptr;
				UWorld* TickWorld = (Pawn != nullptr) ? Pawn->GetWorld() : nullptr;

				if (TickLily == nullptr || Pawn == nullptr || TickWorld == nullptr || !Pawn->IsAlive())
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[LILYKEYS] VERDICT: INVALID — Lily's pawn went away mid-test. Run this early in "
						     "a match."));
					Arm->Set(1, ECVF_SetByConsole);
					delete State;
					return false;
				}

				// *** A PAUSE MID-RUN IS AN INVALID RUN, NOT A FAILING ONE. ***
				//
				// IsWorldReadyForKeyPresses() guards the START of the run and nothing guarded the
				// middle, which cost this pass a full investigation: an editor that loses window
				// focus takes an Escape, ATraceHUD raises the pause menu, the world pauses and
				// ATracePlayerController drops every gameplay press on bGameInputSuppressed. The
				// pawn then does not move at all, so the crouch phase measured +0 uu and the
				// harness reported *** FAIL: 'pressing control doesn't make her come down' *** —
				// the user's exact bug, from a frozen game.
				//
				// A frozen pawn and an ignored key are indistinguishable by displacement, so the
				// only honest thing a harness can do is refuse to grade the run.
				if (TickWorld->IsPaused())
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[LILYKEYS] VERDICT: INVALID — the world PAUSED mid-run (arm=%d phase=%d). The "
						     "pause menu takes Escape and a window that loses focus can raise one; a paused "
						     "pawn does not move, which reads exactly like 'the key did nothing'. Nothing is "
						     "being claimed about the fix — re-run with the game window focused."),
						State->Arm, State->Phase);
					Arm->Set(1, ECVF_SetByConsole);
					delete State;
					return false;
				}

				const float ZNow = static_cast<float>(Pawn->GetActorLocation().Z);

				// --- PHASE -1: arm, cast, and let the flight settle -------------------------------
				if (State->Phase == -1)
				{
					// The claim settle. Only arm 1 ever waits here — arm 2 re-enters this phase with
					// the deadline already behind it.
					if (Now < State->PhaseDeadline)
					{
						return true;
					}

					Arm->Set(State->Arm, ECVF_SetByConsole);

					if (UTraceAbilityComponent* Comp = TickLily->GetAbilityComponent())
					{
						Comp->OnHalfTime();   // the framework's own cooldown clear, so arm 2's E is accepted
					}

					GEngine->Exec(TickWorld, TEXT("Trace.SimInput E 0.05"));

					// THE SETTLE IS LOAD-BEARING. The previous arm leaves her hundreds of uu up and
					// falling; measuring the climb from a fall would flatter the fix. ApplyZip arrests
					// her within a tick or two of the cast, and this waits for that.
					State->Phase = 0;
					State->PhaseDeadline = Now + KeyTestSettleSeconds;
					return true;
				}

				// --- PHASE 0: settle, then HOLD JUMP ----------------------------------------------
				if (State->Phase == 0)
				{
					if (Now < State->PhaseDeadline)
					{
						if (TickLily->IsZipping())
						{
							State->bFlew[State->Arm] = true;
						}
						return true;
					}

					State->ZAtHoldStart = ZNow;
					GEngine->Exec(TickWorld, *FString::Printf(TEXT("Trace.SimInput %s %.2f"),
						*State->JumpKeyName, KeyTestHoldSeconds));

					State->Phase = 1;
					State->PhaseDeadline = Now + KeyTestHoldSeconds;
					return true;
				}

				// --- PHASE 1: the key is up. HOLD NOTHING. *** THE COMPLAINT *** ------------------
				if (State->Phase == 1)
				{
					if (Now < State->PhaseDeadline)
					{
						return true;
					}

					State->ZAtRelease = ZNow;
					State->Climb[State->Arm] = State->ZAtRelease - State->ZAtHoldStart;

					State->Phase = 2;
					State->PhaseDeadline = Now + KeyTestCoastSeconds;
					return true;
				}

				// --- PHASE 2: HOLD CROUCH. *** THE OTHER HALF OF THE COMPLAINT *** ----------------
				if (State->Phase == 2)
				{
					if (Now < State->PhaseDeadline)
					{
						return true;
					}

					State->ZAfterCoast = ZNow;
					State->Coast[State->Arm] = State->ZAfterCoast - State->ZAtRelease;

					GEngine->Exec(TickWorld, *FString::Printf(TEXT("Trace.SimInput %s %.2f"),
						*State->CrouchKeyName, KeyTestCrouchSeconds));

					State->Phase = 3;
					State->PhaseDeadline = Now + KeyTestCrouchSeconds;
					return true;
				}

				if (Now < State->PhaseDeadline)
				{
					return true;
				}

				State->Crouched[State->Arm] = ZNow - State->ZAfterCoast;
				State->bRan[State->Arm] = true;

				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Phase = -1;
					return true;
				}

				Arm->Set(1, ECVF_SetByConsole);
				TickLily->OnHalfTime();
				FinishKeyTest(State);
				delete State;
				return false;
			}),
			0.f);
	}

	FAutoConsoleCommand CmdKeyTest(
		TEXT("Trace.Lily.KeyTest"),
		TEXT("DEMO 19 item 4, from the player's chair. Two arms, RED first (Trace.Lily.ZipHoldRelease 0 = the "
		     "shipped bug, not an imitation of it): presses E, HOLDS jump, LETS GO, then HOLDS crouch through "
		     "the real input pipeline and judges the pawn's world Z. Proves 'she stops when I let go', 'crouch "
		     "brings her down' and 'she climbs at half a walk'."),
		FConsoleCommandDelegate::CreateStatic(&RunKeyTest));

	// =============================================================================================
	// Trace.Lily.TapTest — DEMO 20 ITEM 1, THE OWNER'S SENTENCE WORD FOR WORD
	//
	//     "PRESSING SPACE ONCE makes her CONTINUOUSLY move up EVEN AFTER I LET GO."
	//
	// Trace.Lily.KeyTest above holds jump for a full second and then watches one second of coast,
	// and it passes. This one exists because neither of those numbers is the owner's. He said ONCE
	// — a tap — and he said CONTINUOUSLY, which is a claim about a long time, not about one second.
	//
	// THE TWO THINGS THIS MEASURES THAT A ONE-SECOND HOLD AND A ONE-SECOND COAST CANNOT:
	//
	//   1. A TAP SHORTER THAN THE POLL. SampleClimbIntent() runs at 20 Hz, so a 100 ms tap is two
	//      poll intervals: the press is caught by OnJumpPressed() immediately and the release by the
	//      poll up to 50 ms later. A hold of a full second never exercises that seam at all — it
	//      gives the poll twenty chances to see the key down and twenty to see it up.
	//
	//   2. A RESIDUAL CLIMB TOO SLOW TO SEE IN ONE SECOND. "Continuously" is the whole complaint,
	//      and a drift of 30 uu/s hides inside KeyTest's ±100 uu tolerance while still carrying her
	//      75 uu up over the rest of a flight. So the coast here is 2.5 s and the verdict is quoted
	//      as a RATE, which is the only form of the number that can be compared against "climbing".
	//
	// Same two arms, same red-first discipline, same real input pipeline. RED (the latch) must show
	// her still climbing 2.5 s after a 100 ms tap, or this harness has not reproduced the sentence
	// it claims to be testing and says INVALID rather than PASS.
	// =============================================================================================

	struct FLilyTapTestState
	{
		int32 Arm = 0;              // 0 = RED (ZipHoldRelease 0), 1 = GREEN
		int32 Phase = -2;
		double PhaseDeadline = 0.0;
		double ReadyGiveUpAt = 0.0;

		TWeakObjectPtr<UTraceAbilitySetLily> Lily;
		FString JumpKeyName;
		FString CrouchKeyName;

		float ZAtTap = 0.f;
		float ZAfterTap = 0.f;
		float ZAfterCoast = 0.f;

		float Tapped[2]   = { 0.f, 0.f };   // uu moved during the tap itself
		float Coast[2]    = { 0.f, 0.f };   // uu moved in the 2.5 s AFTER the key came up
		float Crouched[2] = { 0.f, 0.f };   // uu moved while crouch was held
		bool bRan[2]      = { false, false };
		bool bFlew[2]     = { false, false };

		// --- WHY THE CROUCH PHASE DID WHAT IT DID ------------------------------------------------
		//
		// A crouch phase that moves the pawn 0 uu has at least four different causes and the verdict
		// line cannot tell them apart: the key never reached the movement component, the flight had
		// already expired so nothing was driving her, she was stood on the floor where PhysWalking
		// discards Z, or the descend branch was reached and produced nothing. These four samples,
		// taken every tick of the crouch phase, separate all of them — and they are the difference
		// between reporting a number and reporting a cause.
		bool bCrouchEverHeld[2]   = { false, false };   // movement component saw the key as a LEVEL
		bool bZipAliveAtCrouch[2] = { false, false };   // she was still flying while crouch was down
		bool bGroundedAtCrouch[2] = { false, false };   // stood on the floor: PhysWalking eats Z
		float MinVelZAtCrouch[2]  = { 0.f, 0.f };       // the most negative Z velocity commanded
	};

	// 0.35 + 0.10 + 2.50 + 0.80 = 3.75 s, comfortably inside the 5 s flight, so nothing here is
	// measured against a Zip that has already expired. THAT IS LOAD-BEARING: a coast that outlived
	// the flight would show her falling and would read as "the release works" for the wrong reason.
	constexpr float TapTestSettleSeconds = 0.35f;
	constexpr float TapTestTapSeconds    = 0.10f;   // ONE TAP — the owner's "once"
	constexpr float TapTestCoastSeconds  = 2.50f;   // *** hold NOTHING and watch ***
	constexpr float TapTestCrouchSeconds = 0.80f;

	void FinishTapTest(FLilyTapTestState* State)
	{
		const TCHAR* const Tag = TEXT("LILYTAP");
		const UTraceSettings& Settings = UTraceSettings::Get();

		const float WalkSpeed    = FMath::Max(1.f, Settings.WalkSpeed);
		const float ClimbSpeed   = WalkSpeed * FMath::Clamp(Settings.LilyZipClimbSpeedScale, 0.1f, 4.f);
		const float DescendSpeed = WalkSpeed * FMath::Clamp(Settings.LilyZipDescendSpeedScale, 0.1f, 4.f);

		// What a still-latched climb would carry her over the coast, and what a real descent is worth.
		const float LatchedCoastClimb = ClimbSpeed * TapTestCoastSeconds;
		const float ExpectedDescend   = DescendSpeed * TapTestCrouchSeconds;

		for (int32 Index = 0; Index < 2; ++Index)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] arm=%-22s  TAP %s %.2fs: %+.0f uu  |  then NOTHING for %.1fs: %+.0f uu (%+.0f uu/s)  |  "
				     "hold crouch %.1fs: %+.0f uu   (flying=%d)"),
				Tag,
				(Index == 1) ? TEXT("GREEN (the fix)") : TEXT("RED (shipped v21 bug)"),
				*State->JumpKeyName, TapTestTapSeconds, State->Tapped[Index],
				TapTestCoastSeconds, State->Coast[Index], State->Coast[Index] / TapTestCoastSeconds,
				TapTestCrouchSeconds, State->Crouched[Index],
				State->bFlew[Index] ? 1 : 0);

			// The four causes, printed whether or not anything failed. A crouch phase that moved
			// nothing is only diagnosable next to these.
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s]     during that crouch: movement saw the key held=%d | still flying=%d | "
				     "stood on the floor=%d | lowest commanded Z velocity %.0f uu/s"),
				Tag,
				State->bCrouchEverHeld[Index] ? 1 : 0,
				State->bZipAliveAtCrouch[Index] ? 1 : 0,
				State->bGroundedAtCrouch[Index] ? 1 : 0,
				State->MinVelZAtCrouch[Index]);
		}

		// ---- RED MUST REPRODUCE THE OWNER'S SENTENCE ----------------------------------------------
		// "Continuously move up even after I let go" is a coast that is still most of a full climb,
		// and "control doesn't make her come down" is a crouch phase that does not descend.
		const bool bRedKeptClimbing  = State->Coast[0] > LatchedCoastClimb * 0.4f;
		const bool bRedIgnoredCrouch = State->Crouched[0] > -ExpectedDescend * 0.1f;

		if (!State->bRan[0] || !State->bRan[1] || !State->bFlew[0] || !State->bFlew[1]
			|| !bRedKeptClimbing || !bRedIgnoredCrouch)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — the red arm did not reproduce 'press once and she keeps going'. "
				     "redRan=%d greenRan=%d flying(red/green)=%d/%d | after the tap she coasted %+.0f uu (the bug "
				     "needs more than %+.0f) | on crouch she moved %+.0f uu (the bug needs it no lower than %+.0f)."),
				Tag, State->bRan[0] ? 1 : 0, State->bRan[1] ? 1 : 0,
				State->bFlew[0] ? 1 : 0, State->bFlew[1] ? 1 : 0,
				State->Coast[0], LatchedCoastClimb * 0.4f,
				State->Crouched[0], -ExpectedDescend * 0.1f);
			return;
		}

		int32 Failed = 0;

		// (1) A TAP STILL DOES SOMETHING. The fix must not have cost her the key: a press that the
		//     poll never sees would leave her hovering, which is a different bug wearing the same
		//     verdict. She should rise for the tap plus at most one 50 ms poll interval of overshoot.
		const float TapFloor = ClimbSpeed * TapTestTapSeconds * 0.5f;
		if (State->Tapped[1] < TapFloor)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: a %.2fs tap of %s moved her only %+.0f uu (expected at least %+.0f). The press "
				     "is no longer reaching the climb — the release fix must not eat the press."),
				Tag, TapTestTapSeconds, *State->JumpKeyName, State->Tapped[1], TapFloor);
		}

		// (2) *** THE SENTENCE. *** "Continuously move up even after I let go."
		//     10% of a latched climb over 2.5 s — 40 uu/s against a 400 uu/s climb. Anything under
		//     that is the 20 Hz poll's overshoot and the half-tick gravity correction, not a climb.
		const float CoastCeiling = LatchedCoastClimb * 0.1f;
		if (State->Coast[1] > CoastCeiling)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: 'pressing space once makes her continuously move up even after I let go' — "
				     "%.1fs after a %.2fs tap she was %+.0f uu higher (%+.0f uu/s), over the %+.0f uu the poll "
				     "and the gravity correction can account for."),
				Tag, TapTestCoastSeconds, TapTestTapSeconds, State->Coast[1],
				State->Coast[1] / TapTestCoastSeconds, CoastCeiling);
		}

		// (3) *** THE OTHER HALF. *** "Pressing control doesn't make her come down."
		if (State->Crouched[1] > -ExpectedDescend * 0.4f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: 'pressing control doesn't make her come down' — %.1fs of %s moved her %+.0f uu, "
				     "expected about %+.0f (%.0f uu/s down)."),
				Tag, TapTestCrouchSeconds, *State->CrouchKeyName, State->Crouched[1],
				-ExpectedDescend, DescendSpeed);
		}

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — ONE %.2fs tap of %s. WITH THE BUG she was still %+.0f uu/s upward "
				     "%.1fs later and crouch moved her %+.0f uu (still up). FIXED she coasted %+.0f uu/s — a "
				     "stop, not a climb — and %s took her DOWN %+.0f uu."),
				Tag, TapTestTapSeconds, *State->JumpKeyName,
				State->Coast[0] / TapTestCoastSeconds, TapTestCoastSeconds, State->Crouched[0],
				State->Coast[1] / TapTestCoastSeconds, *State->CrouchKeyName, State->Crouched[1]);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d of 3 checks failed."), Tag, Failed);
		}
	}

	void RunTapTest()
	{
		const TCHAR* const Tag = TEXT("LILYTAP");

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Lily.ZipHoldRelease"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — Trace.Lily.ZipHoldRelease is not registered, so there is no red arm."),
				Tag);
			return;
		}

		FLilyTapTestState* State = new FLilyTapTestState();
		State->ReadyGiveUpAt = FPlatformTime::Seconds() + 30.0;

		const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();
		const FKey JumpKey   = UserSettings.GetKey(ETraceInputAction::Jump);
		const FKey CrouchKey = UserSettings.GetKey(ETraceInputAction::Crouch);
		if (!JumpKey.IsValid() || !CrouchKey.IsValid())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — jump or crouch is UNBOUND (jump='%s', crouch='%s')."),
				Tag, *JumpKey.ToString(), *CrouchKey.ToString());
			delete State;
			return;
		}
		State->JumpKeyName   = JumpKey.ToString();
		State->CrouchKeyName = CrouchKey.ToString();

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] armed. Two arms, RED (the shipped bug) first. Per arm: E, ONE %.2fs tap of %s, then "
			     "%.1fs holding NOTHING, then %.1fs of %s — judged by the pawn's world Z."),
			Tag, TapTestTapSeconds, *State->JumpKeyName, TapTestCoastSeconds,
			TapTestCrouchSeconds, *State->CrouchKeyName);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, Arm](float /*Delta*/) -> bool
			{
				const double Now = FPlatformTime::Seconds();

				if (State->Phase == -2)
				{
					UWorld* WaitWorld = FindLocalKeyboardWorld();
					if (!IsWorldReadyForKeyPresses(WaitWorld))
					{
						if (Now > State->ReadyGiveUpAt)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[LILYTAP] VERDICT: INVALID — 30s went by with no unpaused match and a local "
								     "pawn. The select screen pauses the world and eats the E."));
							delete State;
							return false;
						}
						return true;
					}

					FString Why;
					UTraceAbilitySetLily* Claimed = MakePlayerIntoLily(WaitWorld, Why);
					if (Claimed == nullptr || Claimed->GetCharacter() == nullptr)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[LILYTAP] VERDICT: INVALID — %s."),
							(Claimed == nullptr) ? *Why : TEXT("Lily has no pawn"));
						delete State;
						return false;
					}

					State->Lily = Claimed;
					State->Phase = -1;
					// Same claim settle as Trace.Lily.KeyTest — see KeyTestClaimSettleSeconds. Arm 1's
					// E was racing the character swap and being dropped, so the red arm never flew.
					State->PhaseDeadline = Now + KeyTestClaimSettleSeconds;
					return true;
				}

				UTraceAbilitySetLily* TickLily = State->Lily.Get();
				ATraceCharacter* Pawn = (TickLily != nullptr) ? TickLily->GetCharacter() : nullptr;
				UWorld* TickWorld = (Pawn != nullptr) ? Pawn->GetWorld() : nullptr;

				if (TickLily == nullptr || Pawn == nullptr || TickWorld == nullptr || !Pawn->IsAlive())
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[LILYTAP] VERDICT: INVALID — Lily's pawn went away mid-test."));
					Arm->Set(1, ECVF_SetByConsole);
					delete State;
					return false;
				}

				// A pause mid-run freezes the pawn, and a frozen pawn is indistinguishable from an
				// ignored key by displacement alone. See the same guard in RunKeyTest for the run
				// that made this necessary.
				if (TickWorld->IsPaused())
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[LILYTAP] VERDICT: INVALID — the world PAUSED mid-run (arm=%d phase=%d). A paused "
						     "pawn does not move, which reads exactly like 'the key did nothing'. Re-run with "
						     "the game window focused."),
						State->Arm, State->Phase);
					Arm->Set(1, ECVF_SetByConsole);
					delete State;
					return false;
				}

				const float ZNow = static_cast<float>(Pawn->GetActorLocation().Z);

				// --- PHASE -1: arm, cast, settle ---------------------------------------------------
				if (State->Phase == -1)
				{
					// The claim settle. Only arm 1 ever waits here.
					if (Now < State->PhaseDeadline)
					{
						return true;
					}

					Arm->Set(State->Arm, ECVF_SetByConsole);

					if (UTraceAbilityComponent* Comp = TickLily->GetAbilityComponent())
					{
						Comp->OnHalfTime();   // clears the E cooldown so arm 2 can cast
					}

					GEngine->Exec(TickWorld, TEXT("Trace.SimInput E 0.05"));

					State->Phase = 0;
					State->PhaseDeadline = Now + TapTestSettleSeconds;
					return true;
				}

				// --- PHASE 0: settle, then ONE TAP -------------------------------------------------
				if (State->Phase == 0)
				{
					if (Now < State->PhaseDeadline)
					{
						if (TickLily->IsZipping())
						{
							State->bFlew[State->Arm] = true;
						}
						return true;
					}

					State->ZAtTap = ZNow;
					GEngine->Exec(TickWorld, *FString::Printf(TEXT("Trace.SimInput %s %.2f"),
						*State->JumpKeyName, TapTestTapSeconds));

					State->Phase = 1;
					State->PhaseDeadline = Now + TapTestTapSeconds;
					return true;
				}

				// --- PHASE 1: the key is up. HOLD NOTHING. *** THE OWNER'S SENTENCE *** ------------
				if (State->Phase == 1)
				{
					if (Now < State->PhaseDeadline)
					{
						return true;
					}

					State->ZAfterTap = ZNow;
					State->Tapped[State->Arm] = State->ZAfterTap - State->ZAtTap;

					State->Phase = 2;
					State->PhaseDeadline = Now + TapTestCoastSeconds;
					return true;
				}

				// --- PHASE 2: HOLD CROUCH ----------------------------------------------------------
				if (State->Phase == 2)
				{
					if (Now < State->PhaseDeadline)
					{
						return true;
					}

					State->ZAfterCoast = ZNow;
					State->Coast[State->Arm] = State->ZAfterCoast - State->ZAfterTap;

					GEngine->Exec(TickWorld, *FString::Printf(TEXT("Trace.SimInput %s %.2f"),
						*State->CrouchKeyName, TapTestCrouchSeconds));

					State->Phase = 3;
					State->PhaseDeadline = Now + TapTestCrouchSeconds;
					return true;
				}

				// --- PHASE 3: crouch is down. SAMPLE THE CAUSE, not just the displacement. --------
				if (Now < State->PhaseDeadline)
				{
					if (const UTraceCharacterMovementComponent* CrouchMove = TickLily->GetMovement())
					{
						if (CrouchMove->IsCrouchHeld())
						{
							State->bCrouchEverHeld[State->Arm] = true;
						}
						if (CrouchMove->IsMovingOnGround())
						{
							State->bGroundedAtCrouch[State->Arm] = true;
						}
						State->MinVelZAtCrouch[State->Arm] =
							FMath::Min(State->MinVelZAtCrouch[State->Arm], static_cast<float>(CrouchMove->Velocity.Z));
					}
					if (TickLily->IsZipping())
					{
						State->bZipAliveAtCrouch[State->Arm] = true;
					}
					return true;
				}

				State->Crouched[State->Arm] = ZNow - State->ZAfterCoast;
				State->bRan[State->Arm] = true;

				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Phase = -1;
					return true;
				}

				Arm->Set(1, ECVF_SetByConsole);
				TickLily->OnHalfTime();
				FinishTapTest(State);
				delete State;
				return false;
			}),
			0.f);
	}

	FAutoConsoleCommand CmdTapTest(
		TEXT("Trace.Lily.TapTest"),
		TEXT("DEMO 20 item 1, the owner's sentence word for word: ONE 0.10s tap of jump, then 2.5s holding "
		     "NOTHING, then crouch. Two arms, RED first. Where Trace.Lily.KeyTest holds the key for a second, "
		     "this one presses it ONCE and quotes the coast as a RATE, because 'continuously' is a claim about "
		     "a rate and not about one second."),
		FConsoleCommandDelegate::CreateStatic(&RunTapTest));

	// =============================================================================================
	// Trace.Lily.DashTest — DEMO 19 ITEM 8, MEASURED ON THE POOL THE HUD DRAWS
	//
	// "Change Lily so she only has an extra dash when she is not carrying the core."
	//
	// The number asked for is UTraceCharacterMovementComponent::GetMaxDashCharges() — the same call
	// ATracePlayerController::GetDashHudState() makes to fill the dash meter, so this reads exactly
	// the number of pips the player sees. It is taken with the Core in her hands and again without,
	// in both arms, from the real Core actor rather than from a fake carrier bit.
	//
	// TWO ARMS, RED FIRST. Trace.Lily.DashCarrierGate 0 is the shipped v21 rule; if the red arm does
	// not print 3 while carrying then this harness is not measuring the gate and says so.
	//
	// SYNCHRONOUS, like ZipVerify: ATraceCore::TryPickup is an authority-side grant that lands in the
	// same frame, and GetMaxDashCharges() is a pure read.
	// =============================================================================================

	void RunDashTest()
	{
		const TCHAR* const Tag = TEXT("LILYDASH");

		UWorld* TestWorld = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && Context.World()->IsGameWorld()
					&& Context.World()->GetAuthGameMode() != nullptr)
				{
					TestWorld = Context.World();
					break;
				}
			}
		}

		if (TestWorld == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — no authoritative game world. Run this on the server/host."), Tag);
			return;
		}

		FString Why;
		UTraceAbilitySetLily* Lily = MakePlayerIntoLily(TestWorld, Why);
		ATraceCharacter* MyPawn = (Lily != nullptr) ? Lily->GetCharacter() : nullptr;
		ATraceCore* CoreActor = ATraceCore::Get(TestWorld);
		UTraceCharacterMovementComponent* MoveComp = (MyPawn != nullptr) ? MyPawn->GetTraceMovement() : nullptr;

		if (Lily == nullptr || MyPawn == nullptr || CoreActor == nullptr || MoveComp == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — %s."), Tag,
				(Lily == nullptr) ? *Why : TEXT("Lily has no pawn, no movement component, or there is no Core"));
			return;
		}

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Lily.DashCarrierGate"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — Trace.Lily.DashCarrierGate is not registered, so there is no red arm."),
				Tag);
			return;
		}
		const int32 ArmBefore = Arm->GetInt();

		// Somewhere to park the Core so "not carrying" is a real state and not a wish.
		ATraceCharacter* Parker = nullptr;
		for (TActorIterator<ATraceCharacter> It(TestWorld); It; ++It)
		{
			if (*It != nullptr && *It != MyPawn && (*It)->IsAlive()) { Parker = *It; break; }
		}

		int32 Free[2] = { 0, 0 };
		int32 Carrying[2] = { 0, 0 };

		for (int32 ArmIndex = 0; ArmIndex < 2; ++ArmIndex)
		{
			Arm->Set(ArmIndex, ECVF_SetByConsole);   // 0 = RED (ungated), 1 = GREEN (item 8)

			if (Parker != nullptr)
			{
				CoreActor->TryPickup(Parker);
			}
			Free[ArmIndex] = MoveComp->GetMaxDashCharges();

			CoreActor->TryPickup(MyPawn);
			Carrying[ArmIndex] = MoveComp->GetMaxDashCharges();
		}

		if (Parker != nullptr)
		{
			CoreActor->TryPickup(Parker);   // leave the match roughly as it was found
		}
		Arm->Set(ArmBefore, ECVF_SetByConsole);

		const UTraceSettings& Settings = UTraceSettings::Get();
		const int32 EverybodyFree     = FMath::Max(1, Settings.BaseDashCharges);
		const int32 EverybodyCarrying = EverybodyFree + FMath::Max(0, Settings.CarrierExtraDashCharges);

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] RED (gate off, the v21 rule)  Lily %d free / %d carrying   |   "
			     "GREEN (item 8)  Lily %d free / %d carrying   |   everybody else %d / %d."),
			Tag, Free[0], Carrying[0], Free[1], Carrying[1], EverybodyFree, EverybodyCarrying);

		// ---- THE RED ARM FIRST ---------------------------------------------------------------------
		if (Free[0] != EverybodyFree + 1 || Carrying[0] != EverybodyCarrying + 1)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — with the gate OFF she should show the old rule, %d free and %d "
				     "carrying, and she showed %d and %d. The harness is not measuring the gate."),
				Tag, EverybodyFree + 1, EverybodyCarrying + 1, Free[0], Carrying[0]);
			return;
		}

		int32 Failed = 0;
		if (Free[1] != EverybodyFree + 1)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: item 8 keeps her extra dash while she is NOT carrying — expected %d, got %d."),
				Tag, EverybodyFree + 1, Free[1]);
		}
		if (Carrying[1] != EverybodyCarrying)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: 'only ... when she is not carrying the core' — carrying, she should be on the "
				     "same %d as everybody else and she is on %d. (%d would mean the gate did nothing; %d would "
				     "mean it took the carrier's charge away too.)"),
				Tag, EverybodyCarrying, Carrying[1], EverybodyCarrying + 1, EverybodyCarrying - 1);
		}

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — the SAME pickup, measured on the pool the dash meter draws: with the "
				     "old rule %d carrying, with item 8 %d. Free-running she keeps her %d either way."),
				Tag, Carrying[0], Carrying[1], Free[1]);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d of 2 checks failed."), Tag, Failed);
		}
	}

	FAutoConsoleCommand CmdDashTest(
		TEXT("Trace.Lily.DashTest"),
		TEXT("DEMO 19 item 8. Two arms, RED first (Trace.Lily.DashCarrierGate 0 = the shipped v21 rule): hands "
		     "Lily the real Core and reads GetMaxDashCharges() — the same number the dash meter draws — with "
		     "and without it. Proves her extra charge is hers only when she is NOT carrying."),
		FConsoleCommandDelegate::CreateStatic(&RunDashTest));

	/** The numbers §3 states, checked against the knobs, plus an honest note about what is not wired. */
	void RunLilyVerify()
	{
		const TCHAR* const Tag = TEXT("LILY");
		const UTraceSettings& Settings = UTraceSettings::Get();

		int32 Passed = 0;
		int32 Failed = 0;
		auto Check = [&](bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; }
			else { ++Failed; UE_LOG(LogTraceGame, Error, TEXT("[%s] FAIL: %s"), Tag, *What); }
		};

		Check(Settings.LilyExtraDashCharges == 1,
			FString::Printf(TEXT("§3 says ONE extra dash; the knob is %d"), Settings.LilyExtraDashCharges));
		Check(FMath::IsNearlyEqual(Settings.LilyMaxHealth, 60.f, 0.01f),
			FString::Printf(TEXT("§3 says 60 health; the knob is %.1f"), Settings.LilyMaxHealth));
		Check(FMath::IsNearlyEqual(Settings.LilyWallJumpMomentumBonus, 0.3f, 0.001f),
			FString::Printf(TEXT("§3 says +30%% wall-jump momentum; the knob is %.3f"), Settings.LilyWallJumpMomentumBonus));
		Check(FMath::IsNearlyEqual(Settings.LilyZipDurationSeconds, 5.f, 0.01f),
			FString::Printf(TEXT("§3 says Zip lasts 5s; the knob is %.2f"), Settings.LilyZipDurationSeconds));
		Check(FMath::IsNearlyEqual(Settings.LilyZipCooldownSeconds, 30.f, 0.01f),
			FString::Printf(TEXT("§3 says a 30s cooldown; the knob is %.2f"), Settings.LilyZipCooldownSeconds));

		// DEMO 19 ITEM 8 in the units a designer reads. Everybody: base, plus one while carrying.
		// Lily: base plus hers while FREE-RUNNING, and base plus the carrier's — not both — while
		// carrying. The point of printing all four numbers is that "she has an extra dash" and "she
		// has an extra dash while carrying" are now different sentences.
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] DASHES  everybody %d free / %d carrying -> Lily %d free / %d carrying "
			     "(item 8: her extra charge is hers only when she is NOT carrying the Core). "
			     "HEALTH  everybody %.0f -> Lily %.0f, i.e. two body shots instead of three. "
			     "WALL JUMP  retention x%.2f, hers alone."),
			Tag,
			Settings.BaseDashCharges,
			Settings.BaseDashCharges + Settings.CarrierExtraDashCharges,
			Settings.BaseDashCharges + Settings.LilyExtraDashCharges,
			Settings.BaseDashCharges + Settings.CarrierExtraDashCharges,
			Settings.MaxHealth, Settings.LilyMaxHealth,
			1.f + Settings.LilyWallJumpMomentumBonus);

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] ZIP  %.2fs, or %.2fs carrying; climbs at %.0f uu/s and descends at %.0f uu/s "
			     "(WalkSpeed %.0f); %.0fs cooldown."),
			Tag, Settings.LilyZipDurationSeconds,
			Settings.LilyZipDurationSeconds * Settings.LilyZipCarrierDurationScale,
			Settings.WalkSpeed * Settings.LilyZipClimbSpeedScale,
			Settings.WalkSpeed * Settings.LilyZipDescendSpeedScale,
			Settings.WalkSpeed, Settings.LilyZipCooldownSeconds);

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] ALL FOUR ARE LIVE. The extra charge is read by GetMaxDashCharges(), the 60 health by "
			     "TraceHealthComponent::GetMaxHealth(), the wall-jump bonus by TryWallJump()'s retention term, "
			     "and Zip runs in this file. (This line used to say the first three were wired to nothing; it "
			     "was true when it was written and stopped being true without being edited.)"),
			Tag);

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[%s] VERDICT: PASS — %d checks, 0 failed."), Tag, Passed);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d passed, %d FAILED."), Tag, Passed, Failed);
		}
	}

	FAutoConsoleCommand CmdLilyVerify(
		TEXT("Trace.Lily.Verify"),
		TEXT("SPEC v19 §3. Checks Lily's knobs against the doc's numbers, prints the derived flight speeds, and "
		     "says plainly which of her abilities are not yet wired to anything."),
		FConsoleCommandDelegate::CreateStatic(&RunLilyVerify));
}

// =================================================================================================
// Trace.Lily.ZipFxTest — FX_AUDIO_PLAN §2.1, EVERY ELEMENT, AND BOTH DETACH RULES
//
// WHAT IT MEASURES AND WHY EACH MEASUREMENT IS OFF SOMETHING OTHER THAN THIS FILE'S CONSTANTS:
//
//   the cast flash     counted as a FOURTH primitive at t+0.05 and gone by t+0.50, which is the
//                      only way to tell a 0.4 s transient from a piece that was never cleaned up.
//   the flight aura    ring A's height on the body is sampled TWICE and the two must differ — a
//                      static ring passes every "is it there" test ever written and is still wrong.
//   the climb jet      present as a component; its visibility is a function of live velocity and is
//                      reported rather than asserted (a harness cannot make her climb without also
//                      driving the keyboard, and Trace.Lily.KeyTest already owns that).
//   the audio          read out of UTraceAudioSubsystem::GetPlaysByEvent(), i.e. the count the
//                      engine actually took, not "the line is in the file".
//   DEATH detach       counted on the PAWN by component name, so it holds whether or not the set
//                      still exists and whether or not this file's own bookkeeping is honest.
//   SWAP detach        the same census, after ServerSetCharacter(Rocco) — the case where Lily's set
//                      is destroyed and nothing but OnUnequipped can possibly reap her components.
//
// The pawn-side census is the point. DebugFxPrimitiveCount() asks this file what it thinks it owns;
// counting UStaticMeshComponents named LilyZip* on the pawn asks the ENGINE what is attached, and
// only the second one can catch "the pointers were cleared and the components were not".
// =================================================================================================

namespace TraceLilyFxTestFile
{
	/** How many of Lily's FX components are attached to @p Pawn right now, by name. */
	int32 CountFxComponentsOn(const ATraceCharacter* Pawn)
	{
		if (Pawn == nullptr)
		{
			return 0;
		}
		int32 Count = 0;
		for (const UActorComponent* Component : Pawn->GetComponents())
		{
			if (Component != nullptr && Component->GetName().StartsWith(TEXT("LilyZip")))
			{
				++Count;
			}
		}
		return Count;
	}

	/**
	 * Asks for a frame in EXACTLY ATraceHUD's TraceAutoShot format — the harvest scripts grep for
	 * that line, and a second spelling of it is a frame nobody collects. Same helper, same words, as
	 * ATraceFxBurst's RequestFrame and Trace.Chut.BashFxTest's.
	 */
	void RequestZipFrame(UWorld* WorldPtr, const TCHAR* Label)
	{
		if (WorldPtr == nullptr)
		{
			return;
		}

		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Screenshots")
			/ FString::Printf(TEXT("TraceAutoShot_lily_%s_%s.png"), Label,
				*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));

		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(Path));
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Display, TEXT("[AutoShot] Screenshot requested: %s"), *Path);

		if (APlayerController* PC = WorldPtr->GetFirstPlayerController())
		{
			FVector ViewLocation = FVector::ZeroVector;
			FRotator ViewRotation = FRotator::ZeroRotator;
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
			UE_LOG(LogTraceGame, Display,
				TEXT("[AutoShot] View: map=%s pawn=%s at %s | camera %s rot %s"),
				*WorldPtr->GetMapName(), *GetNameSafe(PC->GetPawn()),
				PC->GetPawn() ? *PC->GetPawn()->GetActorLocation().ToCompactString() : TEXT("<none>"),
				*ViewLocation.ToCompactString(), *ViewRotation.ToCompactString());
		}
	}

	int32 PlaysOf(UWorld* WorldPtr, FName Event)
	{
		const UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(WorldPtr);
		if (Audio == nullptr)
		{
			return -1;
		}
		const int32* Found = Audio->GetPlaysByEvent().Find(Event);
		return (Found != nullptr) ? *Found : 0;
	}

	struct FZipFxRun
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		int32 Passed = 0;
		int32 Failed = 0;

		TWeakObjectPtr<UTraceAbilityComponent> Comp;
		TWeakObjectPtr<ATraceCharacter> SwapPawn;

		float RingZFirstSample = 0.f;
		int32 ZipPlaysAtStart = 0;
		int32 LoopPlaysAtStart = 0;

		/** The death arm is judged once; re-entering its step while she respawns is a wait, not a test. */
		bool bDeathArmJudged = false;

		void Check(bool bCondition, const FString& Label, const FString& Detail)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[ZipFx]   %s %s — %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *Label, *Detail);
		}
	};

	/**
	 * Ring A's height on the body, or a sentinel when the ring is not there.
	 *
	 * OFF A BEAD, not off the component. The ring is one UInstancedStaticMeshComponent parked at the
	 * pawn's origin and its beads carry the height, so the component's own Z is 0 for the whole
	 * flight — sampling it would report a wash that never moves, on a wash that does.
	 */
	float SampleRingZ(const ATraceCharacter* Pawn)
	{
		if (Pawn == nullptr)
		{
			return -1000.f;
		}
		for (const UActorComponent* Component : Pawn->GetComponents())
		{
			const UInstancedStaticMeshComponent* Ring = Cast<UInstancedStaticMeshComponent>(Component);
			if (Ring != nullptr && Ring->GetName().StartsWith(TEXT("LilyZipAuraA")))
			{
				FTransform Bead;
				if (Ring->GetInstanceCount() > 0 && Ring->GetInstanceTransform(0, Bead, /*bWorldSpace=*/false))
				{
					return static_cast<float>(Bead.GetLocation().Z);
				}
			}
		}
		return -1000.f;
	}

	/** Is the climb jet actually being DRAWN on @p Pawn? Read off the component, not off intent. */
	bool IsJetVisible(const ATraceCharacter* Pawn)
	{
		if (Pawn == nullptr)
		{
			return false;
		}
		for (const UActorComponent* Component : Pawn->GetComponents())
		{
			const USceneComponent* Scene = Cast<USceneComponent>(Component);
			if (Scene != nullptr && Scene->GetName().StartsWith(TEXT("LilyZipJet")))
			{
				return Scene->IsVisible();
			}
		}
		return false;
	}

	bool TickZipFxRun(TSharedPtr<FZipFxRun> Run);

	void ScheduleZipFx(TSharedPtr<FZipFxRun> Run, float DelaySeconds)
	{
		Run->NextStepRealTime = FPlatformTime::Seconds() + DelaySeconds;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextStepRealTime)
				{
					return true;   // not yet; keep the ticker alive
				}
				return TickZipFxRun(Run);   // false = this step scheduled the next one (or finished)
			}), 0.f);
	}

	bool TickZipFxRun(TSharedPtr<FZipFxRun> Run)
	{
		UWorld* WorldPtr = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetNetMode() != NM_Client)
			{
				WorldPtr = Candidate;
				break;
			}
		}
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ZipFx] the game world went away mid-run."));
			return false;
		}

		UTraceAbilityComponent* const Comp = Run->Comp.Get();
		UTraceAbilitySetLily* const Lily = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetLily>() : nullptr;
		ATraceCharacter* const Pawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;

		switch (Run->Step)
		{
		case 10:
		{
			// THE BEFORE FRAME, after the 0.35 s camera blend into third person has settled.
			// Requesting it at command time would photograph the inside of her head.
			if (Lily == nullptr || Pawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ZipFx] lost Lily before the cast."));
				return false;
			}

			RequestZipFrame(WorldPtr, TEXT("before"));

			// *** AND THEN WAIT TWO SECONDS BEFORE CASTING. ***
			//
			// A SCREENSHOT COSTS A WHOLE FRAME — about 1.1 s of one under -RenderOffScreen at
			// 1728x1117 (measured: Saved/Logs/release/W4-KITS-A-chut1.log requests a frame on engine
			// frame 521 and gets its next tick on frame 522, 1.14 s later). Casting on that frame
			// would put the "t + 0.05 s" sample below at t + 1.15 s instead — past the end of a 0.4 s
			// cast flash — and the harness would report a flash that never existed rather than a
			// screenshot that ate the frame it was measured in.
			Run->Step = 11;
			ScheduleZipFx(Run, 2.0f);
			return false;
		}

		case 11:
		{
			if (Lily == nullptr || Pawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ZipFx] lost Lily before the cast."));
				return false;
			}

			// DIRECTLY, not through the E press. ActivateAbility IS the shipping apply path — what
			// this skips is the framework's cooldown/alive/half-time gate above it, and that gate is
			// what Trace.Lily.ZipVerify and Trace.Lily.KeyTest are for. A harness that had to wait
			// out a 30 s cooldown between arms is a harness nobody runs twice.
			Lily->ActivateAbility();

			// AND HOLD SPACE. The climb jet is the one §2.1 element whose visibility is a function of
			// what the PLAYER is doing — "visible only while ascending (VelZ > 60 uu/s)" — so a
			// harness that only casts photographs a Lily who is hovering, prints the jet as
			// "(hidden)", and calls that a run in which every element fired. Trace.SimInput is the
			// same key-injection road Trace.Lily.KeyTest drives the release bug down.
			if (GEngine != nullptr)
			{
				GEngine->Exec(WorldPtr, TEXT("Trace.SimInput SpaceBar 4.00"));
			}

			Run->Step = 0;
			ScheduleZipFx(Run, 0.05f);
			return false;
		}

		case 0:
		{
			// t + 0.05 s: the cast has just landed. FOUR pieces — the three loop pieces and the
			// flash — and the loop sound running.
			if (Lily == nullptr || Pawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ZipFx] lost Lily before the first sample."));
				return false;
			}

			UE_LOG(LogTraceGame, Display, TEXT("[ZipFx] t+0.05: %s"), *Lily->DebugFxReport());

			// EVERY MEASUREMENT FIRST, THE SCREENSHOT LAST — see case 10 for the 1.1 s frame a
			// screenshot costs. A check taken after the request is a check taken a second later than
			// it says it is.
			const int32 OnPawn = CountFxComponentsOn(Pawn);
			Run->Check(OnPawn == 4, TEXT("all four pieces attached"),
				FString::Printf(TEXT("%d LilyZip* components on %s (aura x2 + jet + cast flash)"),
					OnPawn, *GetNameSafe(Pawn)));
			Run->Check(Lily->DebugFxPrimitiveCount() == OnPawn, TEXT("bookkeeping matches the pawn"),
				FString::Printf(TEXT("the set says %d, the pawn carries %d"),
					Lily->DebugFxPrimitiveCount(), OnPawn));

			const int32 ZipPlays = PlaysOf(WorldPtr, TraceSoundEvents::LilyZip);
			Run->Check(ZipPlays > Run->ZipPlaysAtStart, TEXT("LilyZip one-shot"),
				FString::Printf(TEXT("plays %d -> %d (World side, server multicast)"),
					Run->ZipPlaysAtStart, ZipPlays));

			const int32 LoopPlays = PlaysOf(WorldPtr, TraceSoundEvents::LilyZipLoop);
			Run->Check(LoopPlays > Run->LoopPlaysAtStart, TEXT("LilyZipLoop attached"),
				FString::Printf(TEXT("plays %d -> %d (StartLoopOn, local, no RPC)"),
					Run->LoopPlaysAtStart, LoopPlays));

			Run->RingZFirstSample = SampleRingZ(Pawn);
			Run->Step = 12;
			ScheduleZipFx(Run, 0.20f);
			return false;
		}

		case 12:
		{
			// t + 0.25 s: THE MOTION CHECK, and it is its own step precisely so that no screenshot
			// sits between its two samples. 200 ms of a 700 ms travel is 29% of the path — far more
			// than the 1 uu tolerance below and far less than a full wrap, so a static ring cannot
			// pass by accident and a moving one cannot fail by landing back where it started.
			if (Lily == nullptr || Pawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ZipFx] lost Lily mid-flight."));
				return false;
			}

			const float RingZNow = SampleRingZ(Pawn);
			Run->Check(!FMath::IsNearlyEqual(RingZNow, Run->RingZFirstSample, 1.f), TEXT("the wash is moving"),
				FString::Printf(TEXT("aura ring A at z %.1f, was z %.1f 200 ms ago"),
					RingZNow, Run->RingZFirstSample));

			// THE CAST FRAME: the flash is 40 uu across at the cast and 150 uu at 0.4 s, so this is
			// the moment it and the aura are both on screen and both still bright.
			RequestZipFrame(WorldPtr, TEXT("cast"));

			Run->Step = 1;
			ScheduleZipFx(Run, 1.6f);   // 1.1 s of that is the screenshot's own frame
			return false;
		}

		case 1:
		{
			// The flash is a 0.4 s transient and is long gone by now, so what must be left is the
			// LOOP: three pieces and no fourth.
			if (Lily == nullptr || Pawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ZipFx] lost Lily mid-flight."));
				return false;
			}

			UE_LOG(LogTraceGame, Display, TEXT("[ZipFx] mid-flight: %s"), *Lily->DebugFxReport());

			const int32 OnPawn = CountFxComponentsOn(Pawn);
			Run->Check(OnPawn == 3, TEXT("cast flash expired"),
				FString::Printf(TEXT("%d LilyZip* components left (the 0.4 s flash is gone, the loop is not)"),
					OnPawn));

			// THE CLIMB JET, judged on VISIBILITY and not on existence. It is attached for the whole
			// flight and drawn only while she is rising, so "the component is there" is not the
			// claim §2.1 makes about it.
			Run->Check(IsJetVisible(Pawn), TEXT("climb jet drawn while climbing"),
				FString::Printf(TEXT("VelZ %.0f uu/s (threshold 60), jet %s"),
					Pawn->GetVelocity().Z, IsJetVisible(Pawn) ? TEXT("VISIBLE") : TEXT("hidden")));

			// THE FLIGHT FRAME: the loop aura alone — the picture that has to read for the whole
			// five seconds rather than for the first four hundred milliseconds of them.
			RequestZipFrame(WorldPtr, TEXT("aura"));

			Run->Step = 13;
			ScheduleZipFx(Run, 1.6f);
			return false;
		}

		case 13:
		{
			// --- the DEATH detach -----------------------------------------------------------------
			//
			// RE-CAST IF THE FLIGHT RAN OUT. Two screenshots have cost ~2.2 s of the flight by now
			// and a CARRYING Lily only gets 2.5 s of it, so "she was still flying when I killed her"
			// is a thing to establish rather than assume — a death detach measured on a Lily who had
			// already landed proves nothing at all.
			if (Lily == nullptr || Pawn == nullptr || !Pawn->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ZipFx] lost Lily before the death arm."));
				return false;
			}

			if (CountFxComponentsOn(Pawn) == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[ZipFx] the flight ended while the frames were being taken — re-casting so the "
					     "death arm kills a Lily who is actually flying."));
				Lily->ActivateAbility();
				Run->Step = 13;
				ScheduleZipFx(Run, 0.15f);
				return false;
			}

			if (UTraceHealthComponent* Health = Pawn->Health.Get())
			{
				UE_LOG(LogTraceGame, Display, TEXT("[ZipFx] --- killing her mid-flight (%d pieces up) ---"),
					CountFxComponentsOn(Pawn));
				Run->SwapPawn = Pawn;
				Health->ApplyDamage(100000.f, nullptr, FName(TEXT("Trace.Lily.ZipFxTest")));
			}

			Run->Step = 2;
			ScheduleZipFx(Run, 0.35f);
			return false;
		}

		case 2:
		{
			// IMMEDIATE, not the 0.3 s dissolve: 350 ms is longer than the fade would have been, so
			// this check cannot pass merely because the fade finished — but the report line prints
			// the count so a fade that is still running is visible rather than inferred.
			// JUDGED ONCE. This case is re-entered while she waits out the respawn delay, and a check
			// that re-emits on every retry inflates the pass count with copies of one measurement —
			// the first run of this harness printed the death arm five times and called it ten
			// checks. The retry is a wait, not a re-test.
			if (!Run->bDeathArmJudged)
			{
				Run->bDeathArmJudged = true;

				ATraceCharacter* const Corpse = Run->SwapPawn.Get();
				const int32 OnCorpse = CountFxComponentsOn(Corpse);

				Run->Check(OnCorpse == 0, TEXT("DEATH detach"),
					(Corpse != nullptr)
						? FString::Printf(TEXT("%d LilyZip* components left on the corpse %s"),
							OnCorpse, *GetNameSafe(Corpse))
						: TEXT("the pawn was destroyed outright, which takes its components with it"));

				if (Lily != nullptr)
				{
					Run->Check(Lily->DebugFxPrimitiveCount() == 0, TEXT("death cleared the set's own handles"),
						*Lily->DebugFxReport());
				}
			}

			// --- re-arm for the SWAP detach ---------------------------------------------------------
			if (Lily == nullptr || Pawn == nullptr || !Pawn->IsAlive())
			{
				// She has not respawned yet. Wait rather than declaring the swap arm skipped — the
				// respawn delay is a match rule, not a fixture failure.
				Run->Step = 2;
				ScheduleZipFx(Run, 0.5f);
				return false;
			}

			UE_LOG(LogTraceGame, Display, TEXT("[ZipFx] --- re-zipping, then swapping her away ---"));
			Lily->ActivateAbility();
			Run->SwapPawn = Pawn;
			Run->Step = 3;
			ScheduleZipFx(Run, 0.15f);
			return false;
		}

		case 3:
		{
			ATraceCharacter* const FlyingPawn = Run->SwapPawn.Get();
			const int32 Before = CountFxComponentsOn(FlyingPawn);
			Run->Check(Before > 0, TEXT("re-zip rebuilt the aura"),
				FString::Printf(TEXT("%d LilyZip* components before the swap"), Before));

			if (Comp != nullptr)
			{
				Comp->ServerSetCharacter(ETraceCharacterId::Rocco);
			}

			Run->Step = 4;
			ScheduleZipFx(Run, 0.4f);
			return false;
		}

		case 4:
		default:
		{
			ATraceCharacter* const FormerlyLily = Run->SwapPawn.Get();
			const int32 OnPawn = CountFxComponentsOn(FormerlyLily);

			Run->Check(OnPawn == 0, TEXT("SWAP detach"),
				(FormerlyLily != nullptr)
					? FString::Printf(TEXT("%d LilyZip* components left on %s after ServerSetCharacter(Rocco); "
						"OnUnequipped is the only hook that could have reaped them"),
						OnPawn, *GetNameSafe(FormerlyLily))
					: TEXT("the pawn was replaced by the swap, which takes its components with it"));

			if (Run->Failed == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[ZipFx] VERDICT: PASS — %d checks, 0 failed."), Run->Passed);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[ZipFx] VERDICT: *** FAIL *** — %d passed, %d FAILED."),
					Run->Passed, Run->Failed);
			}
			return false;
		}
		}
	}

	void RunZipFxTest()
	{
		UWorld* WorldPtr = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetNetMode() != NM_Client)
			{
				WorldPtr = Candidate;
				break;
			}
		}

		FString Why;
		UTraceAbilitySetLily* const Lily = TraceLilyVerifyFile::MakePlayerIntoLily(WorldPtr, Why);
		if (Lily == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ZipFx] cannot run: %s."), *Why);
			return;
		}

		UTraceAbilityComponent* const Comp = Lily->GetAbilityComponent();
		ATraceCharacter* const Pawn = Lily->GetCharacter();
		if (Comp == nullptr || Pawn == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ZipFx] Lily has no pawn yet."));
			return;
		}

		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[ZipFx] ===== FX_AUDIO_PLAN §2.1 ===== Zip %.1fs (carrier x%.2f), cooldown %.0fs. FX: aura "
			     "r 46 uu x2 drifting over 0.7 s, jet base r 22 uu / h 60 uu above VelZ 60, cast ring 40 -> "
			     "150 uu over 0.4 s. Rings are 24 emissive beads at 13%% of the ring radius (>= 8 uu "
			     "across, bible §3.4), not §2.1's 3 uu cylinder shells — see the report."),
			Settings.LilyZipDurationSeconds, Settings.LilyZipCarrierDurationScale, Settings.LilyZipCooldownSeconds);

		TSharedPtr<FZipFxRun> Run = MakeShared<FZipFxRun>();
		Run->Comp = Comp;
		Run->ZipPlaysAtStart = PlaysOf(WorldPtr, TraceSoundEvents::LilyZip);
		Run->LoopPlaysAtStart = PlaysOf(WorldPtr, TraceSoundEvents::LilyZipLoop);

		// THIRD PERSON, FORCED. Her whole §2.1 presentation is on her BODY, and the owner is in first
		// person — so a capture from her own camera photographs the arena and proves nothing. This is
		// the camera every other player already has of her.
		if (GEngine != nullptr)
		{
			GEngine->Exec(WorldPtr, TEXT("Trace.ForceThirdPerson 1"));
		}

		// 0.6 s: the camera blend is 0.35 s (TraceCharacterLayout::ViewBlendSeconds).
		Run->Step = 10;
		ScheduleZipFx(Run, 0.6f);
	}

	FAutoConsoleCommand CmdZipFxTest(
		TEXT("Trace.Lily.ZipFxTest"),
		TEXT("FX_AUDIO_PLAN §2.1, server only. Casts Zip and MEASURES its presentation off the pawn: four "
		     "primitives at the cast and three once the 0.4 s flash expires, the aura ring's height sampled "
		     "twice to prove the wash is moving, and LilyZip / LilyZipLoop counted out of the audio "
		     "subsystem's own per-event tally. Then proves BOTH detach rules — she is killed mid-flight, and "
		     "then re-zipped and swapped to Rocco — by counting LilyZip* components on the pawn rather than "
		     "by asking this file what it thinks it owns. Takes about 2s."),
		FConsoleCommandDelegate::CreateStatic(&RunZipFxTest));
}

#endif // !UE_BUILD_SHIPPING

#undef LOCTEXT_NAMESPACE
