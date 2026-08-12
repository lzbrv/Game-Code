// Trace — the practice range's world furniture (spec v19 §2).
//
// Three actor types, all of them spawned at runtime by UTracePracticeRangeSubsystem and by NOTHING
// ELSE. That sentence is the whole leak-proofing argument for this file: none of these classes is
// placed in a level, referenced by the arena builder, or named in any .ini, so a real match cannot
// contain one unless somebody deliberately arms the range's gate.
//
//   ATracePracticePad     the interactive floor pads. Walking onto one IS the input — this slice is
//                         not allowed to touch ATracePlayerController, so a key bind was never on
//                         the table, and a pad you can see and walk onto beats a console command
//                         nobody would ever discover.
//
//   ATracePracticePost    a marker under each dummy. It is what ATracePracticeGameMode::
//                         ChoosePlayerStart hands back for a dummy's respawn, which is how "respawn
//                         in place" is the SHIPPED respawn pipeline pointed at a different spot
//                         rather than a second respawn path racing the first.
//
//   ATracePracticeDummyController
//                         possesses an ordinary ATraceCharacter and then does nothing at all. It is
//                         deliberately NOT an ATraceBotController: that class steers, dashes, jumps,
//                         shoots and fires abilities every tick, and "stationary enemy bots that
//                         take damage" is the opposite of all five.
//
// WHY THE DUMMIES ARE ON ETraceTeam::None AND NOT ON THE ENEMY TEAM. Four separate rules fall out of
// it correctly, and every one of them would need special-casing otherwise:
//   * they stay damageable — both the shot resolver (UTraceLagCompensationComponent, "skip when the
//     target's team EQUALS the shooter's") and the ability choke point (UTraceAbilityComponent::
//     CanAffectTargetDetailed, same test) only ever skip a MATCHING team, so None is a legal victim
//     for bullets, melee and every ability;
//   * they stay Mannequins — ATraceGameMode::PollCharacterSelect's bot fill skips any bot whose team
//     is None, so a dummy never draws a random character and never quietly applies Chut's −30%
//     incoming damage to the numbers you are trying to read;
//   * they cannot move the scoreboard — ATraceGameMode::EvaluateWipeBonus returns immediately for
//     ETraceTeam::None, so killing all five never pays a wipe bonus and therefore never walks the
//     match towards the mercy rule;
//   * they cannot receive the Core — ATraceCore::ServerSurfaceTurnover only ever awards to the team
//     OPPOSING the thrower, and nothing opposes None.

#pragma once

#include "CoreMinimal.h"

#include "AIController.h"
#include "GameFramework/Actor.h"
#include "UObject/ObjectPtr.h"

#include "TracePracticeActors.generated.h"

class ATraceCharacter;
class UPrimitiveComponent;
class USphereComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * What a pad does when a player walks onto it.
 *
 * One enum and one actor class rather than three subclasses, because the three differ only in the
 * message they send to the subsystem — every pad is the same disc, the same trigger and the same
 * label, and three classes would be three places to fix the next time the disc changes.
 */
UENUM()
enum class ETracePracticePadRole : uint8
{
	/**
	 * THE NO-TURNOVER SPOT. Spec v19 §2, verbatim: "there is a spot where you can leave the core
	 * without it turning over."
	 *
	 * Sits exactly on ATraceCore::GetHomeLocation() — the point the Core is parked at when it is out
	 * of play — because that is what makes "leave it here" a real state of the shipped Core rather
	 * than a rule this file re-implements. See UTracePracticeRangeSubsystem::HandleCoreRack.
	 */
	CoreRack = 0,

	/** Toggles the range's infinite-abilities switch. Off when the range opens. */
	InfiniteAbilities = 1,

	/** Reopens the character select screen so you can change character without leaving the range. */
	CharacterSwap = 2
};

/**
 * A floor disc you activate by standing on it.
 *
 * The pad reports the ENTRY EDGE only and never the dwell, which is what makes "walk on to put the
 * Core down, walk off and back on to pick it up again" work with no second input: after a deposit
 * you are still standing on the rack, and a dwell-driven pad would immediately hand the Core back.
 */
UCLASS(NotBlueprintable)
class TRACE_API ATracePracticePad : public AActor
{
	GENERATED_BODY()

public:
	ATracePracticePad();

	/** Server-side setup, called by the subsystem immediately after SpawnActor. */
	void ConfigurePad(ETracePracticePadRole InRole, const FString& InLabel);

	ETracePracticePadRole GetPadRole() const { return PadRole; }

	/** Repaints the label without respawning the pad — the infinite-abilities pad flips ON/OFF. */
	void SetPadLabel(const FString& InLabel);

	/** Amber while armed, dim while idle. Purely cosmetic; never read by a rule. */
	void SetPadLit(bool bLit);

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnPadOverlapBegin(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	                       UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	                       bool bFromSweep, const FHitResult& SweepResult);

	UPROPERTY(VisibleAnywhere, Category = "Trace|Practice")
	TObjectPtr<USphereComponent> Trigger;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Practice")
	TObjectPtr<UStaticMeshComponent> Disc;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Practice")
	TObjectPtr<UTextRenderComponent> Label;

	UPROPERTY(Replicated)
	ETracePracticePadRole PadRole = ETracePracticePadRole::CoreRack;

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	/**
	 * World time of the last accepted entry, so one step onto the pad fires once.
	 *
	 * A pawn's capsule and its two hit-zone primitives can all begin overlapping the same trigger in
	 * the same frame; without this a single step onto the Core rack would deposit the Core and then
	 * immediately take it back, which reads to a player as "the pad did nothing".
	 */
	float LastAcceptedWorldTime = -1000.f;
};

/**
 * Where one dummy stands, and where it comes back to.
 *
 * ATracePracticeGameMode::ChoosePlayerStart returns this actor for the dummy that owns it, which is
 * the entire implementation of "respawn in place": the shipped death → RespawnDelay →
 * RestartPlayerFresh → ChoosePlayerStart pipeline runs exactly as it does for a human, and only the
 * answer to "where?" changes. Nothing here duplicates a respawn.
 */
UCLASS(NotBlueprintable)
class TRACE_API ATracePracticePost : public AActor
{
	GENERATED_BODY()

public:
	ATracePracticePost();

protected:
	UPROPERTY(VisibleAnywhere, Category = "Trace|Practice")
	TObjectPtr<UStaticMeshComponent> Disc;
};

/**
 * The controller a practice dummy gets. It possesses, it stands still, and that is all.
 *
 * IT MUST NOT INHERIT FROM ATraceBotController. That class's tick is the whole AI — steering,
 * dashing, jumping, shooting and ability presses — and none of it is switchable off from outside the
 * AI slice, which this pass does not own. A bare AAIController gives us the two things the range
 * actually needs (a PlayerState, so the pawn has a team, a health record and an ability component;
 * and a controller, so the shipped death/respawn pipeline applies) and nothing else.
 */
UCLASS(NotBlueprintable)
class TRACE_API ATracePracticeDummyController : public AAIController
{
	GENERATED_BODY()

public:
	ATracePracticeDummyController();

	virtual void OnPossess(APawn* InPawn) override;

	/** The post this dummy belongs to. Read by ATracePracticeGameMode::ChoosePlayerStart. */
	void SetHomePost(ATracePracticePost* InPost) { HomePost = InPost; }
	ATracePracticePost* GetHomePost() const { return HomePost.Get(); }

private:
	UPROPERTY(Transient)
	TWeakObjectPtr<ATracePracticePost> HomePost;
};
