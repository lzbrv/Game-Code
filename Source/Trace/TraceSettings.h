// Trace — every runtime-tunable number in one place.
//
// UTraceSettings is a UDeveloperSettings, so it shows up under Project Settings and persists to
// Config/DefaultGame.ini (defaultconfig). Read it from anywhere with UTraceSettings::Get().
//
// Rule for the rest of the codebase: never hardcode a gameplay constant that lives in this
// table. Read it through Get() at the point of use — designers change these live, and the CDO
// is refreshed by the config system, so caching a copy in a constructor will go stale.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"   // module: DeveloperSettings
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"     // GetDefault<>

#include "TraceTypes.h"                 // ETrailLethality

#include "TraceSettings.generated.h"

/**
 * Gameplay tuning for Trace.
 *
 * Every member is a `config` property: values ship in DefaultGame.ini under
 * [/Script/Trace.TraceSettings] and can be overridden per-platform or per-user without a rebuild.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Trace Gameplay"))
class TRACE_API UTraceSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UTraceSettings(const FObjectInitializer& ObjectInitializer);

	/**
	 * The one accessor. Returns the class default object, which the config system has already
	 * populated — cheap enough to call per-frame, but do not hold the reference across a
	 * hot reload.
	 */
	static const UTraceSettings& Get();

	/** Groups the page under "Game" in Project Settings rather than the default bucket. */
	virtual FName GetCategoryName() const override;

	// ------------------------------------------------------------------------------------------
	// Match
	// ------------------------------------------------------------------------------------------

	/** Captures needed to win outright. */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	int32 ScoreToWin = 5;

	/** Match length in seconds. Highest score when this expires wins. */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	float MatchDuration = 600.f;

	/** Target roster size per team; used to balance teams on login. */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	int32 PlayersPerTeam = 5;

	/** Connected players required before the match leaves WaitingForPlayers. */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	int32 MinPlayersToStart = 2;

	/** Seconds between death and respawn. */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	float RespawnDelay = 5.f;

	/** Countdown after MinPlayersToStart is met, before the match goes InProgress. */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	float WarmupDuration = 5.f;

	// ------------------------------------------------------------------------------------------
	// Combat
	// ------------------------------------------------------------------------------------------

	/** Starting and maximum health. */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	float MaxHealth = 100.f;

	/** Damage per hitscan body shot. */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	float HitscanDamage = 34.f;

	/** Multiplier applied to HitscanDamage on a headshot. */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	float HeadshotMultiplier = 2.f;

	/** Maximum hitscan distance in unreal units. */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	float HitscanRange = 15000.f;

	/** Seconds between shots. The server validates fire rate against this with a tolerance. */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	float FireInterval = 0.12f;

	/** Off by design: teammates never damage each other. Flip only for tuning experiments. */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	bool bFriendlyFire = false;

	/** Half-angle cone of random spread applied to each shot, in degrees. */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	float SpreadDegrees = 0.6f;

	// ------------------------------------------------------------------------------------------
	// Movement
	// ------------------------------------------------------------------------------------------

	/** Base ground speed. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float WalkSpeed = 720.f;

	/** WalkSpeed multiplier while carrying the Core — the carrier is slightly faster. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float CarrierSpeedMultiplier = 1.08f;

	/** Speed clamp while dashing. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float DashSpeed = 2600.f;

	/** How long a dash lasts. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float DashDuration = 0.18f;

	/** Cooldown measured from dash start. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float DashCooldown = 3.f;

	// ------------------------------------------------------------------------------------------
	// Core
	// ------------------------------------------------------------------------------------------

	/** Launch speed of a thrown/passed Core. */
	UPROPERTY(config, EditAnywhere, Category = "Core")
	float PassSpeed = 2400.f;

	/** Fraction of the throw direction added as +Z, so passes arc instead of skimming the floor. */
	UPROPERTY(config, EditAnywhere, Category = "Core")
	float PassUpwardBias = 0.14f;

	/** Radius within which a character may pick the Core up. */
	UPROPERTY(config, EditAnywhere, Category = "Core")
	float PickupRadius = 110.f;

	/** How long the thrower is blocked from re-catching their own pass. */
	UPROPERTY(config, EditAnywhere, Category = "Core")
	float PickupLockoutAfterThrow = 0.35f;

	/** A loose, untouched Core returns to centre after this many seconds. */
	UPROPERTY(config, EditAnywhere, Category = "Core")
	float CoreResetTime = 15.f;

	// ------------------------------------------------------------------------------------------
	// Trail
	// ------------------------------------------------------------------------------------------

	/** Who dies when the trail is tripped. Default rule: the carrier does. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	ETrailLethality TrailLethality = ETrailLethality::KillsCarrier;

	/** True: teammates of the carrier pass through the trail harmlessly. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	bool bOnlyEnemiesTripTrail = true;

	/** True: only a dashing player trips the trail. This is the core counterplay rule. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	bool bRequireDashToTripTrail = true;

	/** Seconds a trail point survives after being laid. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	float TrailLifetime = 6.f;

	/** Distance the carrier must travel before a new point is appended. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	float TrailPointSpacing = 60.f;

	/** Collision/visual radius of a trail segment. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	float TrailRadius = 45.f;

	/** Collision/visual height of a trail segment. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	float TrailHeight = 190.f;

	/** Hard cap on replicated trail points; oldest are dropped first. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	int32 MaxTrailPoints = 256;

	/** Newest N points are exempt from the trip test so the carrier never kills themselves. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	int32 TrailHeadGracePoints = 3;

	// ------------------------------------------------------------------------------------------
	// Net
	// ------------------------------------------------------------------------------------------

	/** Master switch for server-side rewind on hitscan. Off = resolve against present-day poses. */
	UPROPERTY(config, EditAnywhere, Category = "Net")
	bool bEnableLagCompensation = true;

	/** Upper bound on how far back the server will rewind, in seconds. */
	UPROPERTY(config, EditAnywhere, Category = "Net")
	float MaxRewindTime = 0.25f;

	/** How much pose history each character keeps, in seconds. Must exceed MaxRewindTime. */
	UPROPERTY(config, EditAnywhere, Category = "Net")
	float LagCompHistoryDuration = 1.f;

	/** Draws the rewound capsules the server actually tested against. Noisy; dev only. */
	UPROPERTY(config, EditAnywhere, Category = "Net")
	bool bDrawServerRewindDebug = false;
};
