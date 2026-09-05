// Trace — player controller.
//
// Owns the *entire* Enhanced Input setup. All gameplay input is funnelled through the
// ATraceCharacter::Do* entry points; the controller itself never mutates gameplay state.
//
// TWO WAYS THE INPUT DATA CAN COME INTO EXISTENCE, AND BOTH ARE LIVE (spec v17 §6).
//
//   1. ASSETS — /Game/Trace/Input/IA_*.uasset and IMC_Trace.uasset, written by
//      Scripts/generate-input-assets.py FROM the same action table the C++ path uses. Preferred
//      when they are present, load cleanly and pass validation.
//   2. C++ — NewObject, exactly as this class has always done it. Used when the assets are
//      missing, fail to load, disagree with the C++ table, or when the toggle is off.
//
// The fallback is not a formality: the game plays identically either way, and which path ran is
// stated at Display in every log. `Trace.Input.VerifyAssets` prints the whole comparison.
//
// The project USED to cite "build contract §2: this project cannot author .uassets" as the reason
// everything here was built in code. That contract is retired — the arena bake authored 572 actors
// and 66 materials from a headless commandlet, and these input assets come from the same mechanism.
//
// WHY THE LOADED CONTEXT IS DUPLICATED RATHER THAN USED DIRECTLY, which is the one real hazard of
// the asset path: MapKey()/UnmapAll() MUTATE the context they are called on, and ApplyControlSettings
// rebuilds the whole mapping list on every settings change. Done to a loaded .uasset that mutation
// outlives PIE, dirties the asset, and would be shared by every local player. So the asset is a
// TEMPLATE: DuplicateObject gives this controller its own copy, which dies with it. The IA_* actions
// are NOT duplicated — they are never mutated, and Enhanced Input keys its per-action runtime state
// off the local player, not off the action object, so sharing them is correct.
//
// The one thing that will silently break this file: dropping the UPROPERTY on any IA_* member. A
// NewObject'd UInputAction has no other owner rooting it, so it would be collected at the next GC
// and input would stop working mid-match with no error anywhere.

#pragma once

#include "Camera/CameraModifier.h"       // UTraceViewKickModifier, the §1.5 view kick's carrier
#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"  // FDelegateHandle
#include "Engine/EngineTypes.h"          // EEndPlayReason
#include "GameFramework/PlayerController.h"
#include "Gameplay/TraceHitZones.h"      // ETraceHitZone (ClientNotifyHit's payload)
#include "TraceTypes.h"                  // ETraceTeam (D31-TEAMS — the team-select session)
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

// SPEC v29 §5. HandleDirectEquip now takes the WEAPON rather than a bool, so the enum has to be
// visible here. Forward-declared with its underlying type rather than pulling in the whole of
// Gameplay/TraceMelee.h: nothing in this header needs the knife's policy, only the name of the
// selector, and this file is included by most of the UI slice.
enum class ETraceEquippedWeapon : uint8;

#include "TracePlayerController.generated.h"

/**
 * FX_AUDIO_PLAN §1.5 — the four view kicks, and there are deliberately only four.
 *
 * A kick is the VICTIM's feedback for something that happened TO them: it is sent by the server to
 * one player, it never leaves the camera, and it never means "you did something". The values live in
 * ATracePlayerController::KickValuesFor(), one table, so a designer retunes them in one place.
 *
 * The enum is the RPC's payload (one byte on the wire) rather than three floats for the ordinary
 * reason: three floats sent from the server is three chances for a call site to invent its own feel,
 * and the fifth kick somebody adds would then be untunable from anywhere.
 */
UENUM()
enum class ETraceViewKick : uint8
{
	/** Chut's bash landed on you. 3.5 deg / 0.25 s / 9 Hz — short, sharp, over before you turn. */
	BashVictim = 0,
	/** Mortimer's quake, inside the ring. 4.0 deg / 0.35 s / 7 Hz — the biggest kick in the game. */
	QuakeNear = 1,
	/** Mortimer's quake, outside the ring. 1.5 deg / 0.30 s / 7 Hz — you felt it, you were not in it. */
	QuakeFar = 2,
	/** Your own rocket went off near you. 2.5 deg / 0.30 s / 8 Hz. */
	RocketSelf = 3,
};

/**
 * D31-TEAMS (a). What the server said about a request to change team.
 *
 * Shaped exactly like ETraceCharacterPickResult and for the identical reason spelled out at the top
 * of TracePlayerState.h: a button that silently does nothing is indistinguishable from a dropped
 * packet, and the player's next move is to press it again forever. Every refusal below names WHICH
 * rule refused, because the balance rule in particular is one a player is entitled to argue with.
 */
UENUM()
enum class ETraceTeamChangeResult : uint8
{
	/** Done. The player is on the requested team and has been respawned on that side. */
	Granted = 0,
	/** They already were. Not an error; the screen just closes. */
	AlreadyOnTeam,
	/** THE BALANCE RULE. Taking the slot would leave the sides more than one player apart. */
	WouldUnbalance,
	/** The destination is at PlayersPerTeam and has no bot whose slot could be taken. */
	TeamFull,
	/** Not a team (None), or a bot asked, or the match has no game mode to ask. */
	NotAllowed
};

class APawn;
class ATraceCharacter;
class ATracePlayerState;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

/**
 * Everything the HUD needs to draw the dash indicator, resolved in one call.
 *
 * A struct rather than four out-parameters because the HUD needs all of it at once and the pieces
 * are only meaningful together — a charge count without its recharge fraction cannot be drawn.
 *
 * Plain C++, declared outside the UCLASS: it is never reflected, never replicated and never touched
 * by Blueprint, so putting it through UHT would buy nothing.
 */
struct FTraceDashHudState
{
	/** Charges ready to spend right now. */
	int32 Charges = 0;

	/** Total charges the pawn has. 2 for the Core carrier once spec §5 lands, 1 otherwise. */
	int32 MaxCharges = 1;

	/** 0..1 progress of the charge currently regenerating. 1 when nothing is recharging. */
	float RechargeFraction = 1.f;

	/** Seconds until the next charge lands. 0 when nothing is recharging. */
	float Remaining = 0.f;
};

/**
 * Trace's player controller.
 *
 * Input model. Every key below is the DEFAULT — all of them are rebindable through the options
 * screen, and the authority at runtime is UTraceUserSettings, not this comment. (This list used to
 * cite "contract §7: Canvas only"; that contract is retired and the citation was never relevant to
 * the key table anyway.) The ACTIONS themselves are assets under /Game/Trace/Input when they exist,
 * with the C++ constructors as the live fallback — see the file header:
 *   Move        Axis2D   WASD          -> X = strafe (+right), Y = forward (+forward)
 *   Look        Axis2D   Mouse X / Y   -> X = yaw delta, Y = pitch delta, scaled and signed by
 *                                         the player's sensitivity and invert-Y settings
 *   Jump        bool     Space
 *   Crouch      bool     Left Ctrl     (slide on the ground, fast-fall in the air)
 *   Fire        bool     LMB           (doubles as "put me back in" while dead)
 *   Pass        bool     RMB
 *   Dash        bool     Left Shift
 *   Parry       bool     RMB           (carrier only — 0.175s of trace invulnerability, spec v3 §3 / v8 §3 / v10 §4;
 *                                       moved from Q to right mouse by spec v25 §7)
 *   PullCore    bool     G             (spec v26 §1 — the turnover Core-pull, its OWN bind. Held.
 *                                       It rode the parry's button in v25 §2; §1 splits them, and the
 *                                       old precedence rule survives only as a tiebreak for a player
 *                                       who deliberately puts both actions on one key.
 *                                       SPEC v31 §5 MOVED IT F -> G to free F for the knife flourish,
 *                                       with the "PullCore" -> "PullCoreKey" ConfigId migration. It did
 *                                       not lose a route: v28 §10's precedence still dispatches the
 *                                       pull from the melee button.)
 *   Scoreboard  bool     Tab           (held)
 *   EquipKnife  bool     3             (*** SPEC v31 §1 — THE KNIFE IS A WEAPON SLOT AGAIN. *** The
 *                                       dual-wield switch UTraceMeleeSettings::bDualWieldKnife is OFF,
 *                                       so there is no STOW state: this key draws the blade, one weapon
 *                                       on screen at a time, and it is the knife that pays the v12 §3
 *                                       +22% speed boost. Its pullout is 35% shorter than a gun's
 *                                       (KnifeSwapMultiplier, spent in TraceMelee::GetSwapSecondsFor).
 *                                       DIRECT SELECT, idempotent. The enumerator keeps its v13
 *                                       spelling; only the KEY moved, 1 -> 3.)
 *   EquipGun    bool     1             (spec v31 §1 — the PISTOL, moved 2 -> 1. DIRECT SELECT, idempotent)
 *   EquipSmg    bool     2             (spec v31 §1 — the SMG, moved 3 -> 2. The SWING rides IA_Melee
 *                                       since v28 §10.)
 *   Inspect     bool     F             (spec v31 §5 — the 3.20 s butterfly-knife flourish, on the F that
 *                                       PullCore vacated. Purely cosmetic: it confers and blocks nothing,
 *                                       and any real action interrupts it.)
 *   Reload      bool     R             (spec v16 §1 — the clip also reloads itself when it empties)
 *
 * SPEC v15 §5: TWO WEAPON KEYS, AND NO TOGGLE. Verbatim: "Switch weapon keybind so that it's only
 * switch to knife/switch to gun." The `SwapWeapon` action — gun <-> knife on F, spec v10 §1 — is
 * deleted outright: the enumerator, IA_SwapWeapon, its mapping, its handler and its row in the
 * rebind list. This supersedes spec v13 §2's [ASSUMPTION] that it was worth keeping alongside the
 * two direct selects.
 *
 * The toggle VERB survives one level down as TraceMelee::RequestSwapWeapon, because the weapon
 * component still needs "give me the other one" for the dev console (Trace.Knife.Swap) and for the
 * v13 §2 harness's red arm. What no longer exists is a KEY that means it, which is what was asked.
 */
UCLASS()
class TRACE_API ATracePlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ATracePlayerController();

	//~ Begin AActor / APlayerController interface
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupInputComponent() override;
	/** Server-side possession. */
	virtual void OnPossess(APawn* InPawn) override;
	/** Client-side possession (also runs on a listen-server host). */
	virtual void AcknowledgePossession(APawn* P) override;
	//~ End AActor / APlayerController interface

	/**
	 * Shooter-side hit confirmation, sent by the server once a hitscan resolves.
	 * Reliable: a dropped hit marker reads as a missed shot, which is worse than a late one.
	 *
	 * @param bShieldBlocked C5 (code-gameplay F2). TRUE when the resolve found a victim the damage
	 *        could not touch — the Core carrier's shield. UTraceHealthComponent::ApplyDamage no-ops
	 *        for an invulnerable target, so without this the shooter was sent the same white marker
	 *        for 0 damage as for a real hit: the one piece of feedback in the game that was allowed
	 *        to lie. The fact is DERIVED at the resolve site from the victim's own
	 *        IsInvulnerable() — the same query ApplyDamage consults, never a second carrier test.
	 *
	 * NO DEFAULT ARGUMENT: UFUNCTION RPCs may not have one, so every call site names all three.
	 */
	UFUNCTION(Client, Reliable)
	void ClientNotifyHit(bool bKilled, ETraceHitZone Zone, bool bShieldBlocked);

	/**
	 * FX_AUDIO_PLAN §1.5 — SERVER -> VICTIM: shake their view.
	 *
	 * Reliable, like its neighbour above and for a related reason: a kick is the only confirmation
	 * some abilities give the person on the receiving end (Mortimer's far ring does no damage at all),
	 * and a dropped one reads as an ability that did nothing.
	 *
	 * TAKES THE ENUM, NOT A RAW uint8. §1.5 spells the parameter `uint8 KickType`; the enum is one
	 * byte on the wire just the same and cannot be called with the wrong number. Call it as
	 * `PC->ClientAbilityKick(ETraceViewKick::QuakeNear)`.
	 */
	UFUNCTION(Client, Reliable)
	void ClientAbilityKick(ETraceViewKick KickType);

	/**
	 * LOCAL. Starts (or restarts) the view kick on this machine's camera.
	 *
	 * @param PitchAmplitudeDeg  peak pitch offset in degrees, at t = 0.
	 * @param DurationSeconds    total length; the kick is gone, exactly, at the end of it.
	 * @param NoiseHz            oscillations per second.
	 *
	 * THE SHAPE: Pitch(t) = sin(2*PI*NoiseHz*t) * Amplitude * (1 - t/Duration)^2 — a damped
	 * oscillation that starts at zero, peaks in the first quarter cycle and returns to exactly zero at
	 * the end rather than being cut off. Squared decay because a linear one still has visible motion
	 * when it ends.
	 *
	 * IT NEVER TOUCHES THE AIM. The offset is applied by a UCameraModifier on the camera manager,
	 * i.e. to the POV the camera manager hands the renderer, AFTER the pawn's camera has computed it.
	 * The control rotation — which is what ATraceCharacter::GetAimDirection samples, what the shot ray
	 * is built from and what the server validates against — is not written to at any point. This is
	 * the viewmodel-kick contract (ATraceCharacter::ViewModelKick), not the gun-recoil contract
	 * (UTraceWeaponComponent::ApplyRecoilKick, which deliberately DOES move the aim and is off).
	 *
	 * A second kick during a first replaces it (the loudest recent event wins) rather than summing:
	 * two quakes 50 ms apart must not stack into a 8 degree lurch.
	 */
	void AddViewKick(float PitchAmplitudeDeg, float DurationSeconds, float NoiseHz);

	/**
	 * The kick's pitch offset in degrees RIGHT NOW, or 0 when none is running.
	 *
	 * Read by UTraceViewKickModifier every frame the camera updates, and by the dev harness. Pure: it
	 * is a function of the clock and the three stored values, so nothing has to tick to keep it right
	 * and a paused world holds still.
	 */
	float GetViewKickPitchOffset() const;

	/** The kick table (§1.5): amplitude in degrees, duration in seconds, frequency in Hz. */
	static void KickValuesFor(ETraceViewKick KickType, float& OutAmplitudeDeg, float& OutDurationSeconds,
	                          float& OutNoiseHz);

	/** Which zone the last confirmed hit landed on. Drives the hitmarker's colour and shape. */
	ETraceHitZone GetLastHitMarkerZone() const { return LastHitMarkerZone; }

	/**
	 * D30-RESETS (a) — SERVER -> OWNING CLIENT: throw your momentum away, do not wait to be corrected.
	 *
	 * The owner's rule is "momentum should reset when halves switch and when you respawn". Velocity
	 * and the whole movement kit (dash, slide, wall-jump window, surf ride and its exit carry) are
	 * CLIENT-PREDICTED, so an authority that only zeroes its own copy has not reset anything: the
	 * client keeps simulating from the state it still holds, and the server's zero reaches it as a
	 * position correction it immediately fights. The visible result is a rubber-band on the frame a
	 * half changes, which is the single most-watched frame in the match.
	 *
	 * So the reset is performed on BOTH machines, by the same function —
	 * UTraceCharacterMovementComponent::ResetMomentum — and this RPC is how it reaches the second one.
	 *
	 * Reliable: a dropped reset is a player who keeps 1500 uu/s through half time, which is the bug.
	 *
	 * Takes no arguments on purpose. There is nothing to synchronise: the target state is "what a
	 * freshly spawned pawn has", which both machines can name without being told.
	 *
	 * A LISTEN HOST'S OWN CONTROLLER IGNORES IT. On a listen server this executes in place, on a
	 * controller whose pawn the authority already reset directly; running it twice would be harmless
	 * but would double every [Resets] line in the log and make the host look like it reset twice.
	 */
	UFUNCTION(Client, Reliable)
	void ClientResetMomentum();

	/** Victim-side death notification. Drives the killer line on the death panel. */
	UFUNCTION(Client, Reliable)
	void ClientNotifyKilledBy(const FString& KillerName, FName Cause);

	/**
	 * KILLER-side notification for spec v6 §3: your parry killed the enemy who dashed your trace.
	 *
	 * The dasher already learns why they died through ClientNotifyKilledBy above — the kill carries
	 * cause "Parried" and the carrier as the killer. This is the other half, and it needs its own
	 * message because the carrier's screen shows them nothing at all otherwise: the enemy simply
	 * stops existing somewhere behind them, which is the one outcome a 0.1 s reaction check must
	 * never produce silently.
	 *
	 * Reliable for the same reason ClientNotifyHit is: a dropped confirmation of a hit reads as a
	 * miss, and a parry the carrier cannot confirm reads as a parry that did not register.
	 *
	 * A listen-server host needs none of this — TraceParry keeps the authoritative record and
	 * ATraceHUD reads it directly — but a remote client has no access to that record at all.
	 */
	UFUNCTION(Client, Reliable)
	void ClientNotifyParryKill(const FString& VictimName);

	/** Client-local time of our last parry kill, or a large negative sentinel. See the RPC above. */
	float GetLastParryKillTime() const { return LastParryKillTime; }

	/** Who our last parry killed; empty until the server tells us. */
	const FString& GetLastParryKillVictim() const { return LastParryKillVictim; }

	/** Asks the server to put us back in the game. See the implementation for the exact rules. */
	UFUNCTION(Server, Reliable)
	void ServerRequestRespawn();

	// -----------------------------------------------------------------------------------------
	// D31-TEAMS — the team-select session, and the mid-match character switch
	//
	// WHY THE SESSION LIVES ON THE CONTROLLER AND NOT ON THE PLAYER STATE, which is where the
	// character-select session lives. Two reasons, and the second is the load-bearing one:
	//
	//   1. It is nobody else's business. A PlayerState replicates to EVERY client (the scoreboard
	//      needs it); a PlayerController is bOnlyRelevantToOwner, so a flag here reaches exactly the
	//      one machine that draws the screen and costs nothing on the other nine connections. The
	//      character-select session had to pay for COND_OwnerOnly by hand to get the same property.
	//   2. It is the actor the CLIENT OWNS, so the request RPCs below are legal on it. That is not a
	//      convenience: a Server RPC may only be sent on an actor the sending connection owns, which
	//      is exactly why ATracePlayerState::ServerRequestCharacter had to justify itself in its own
	//      comment (AController::InitPlayerState makes the controller the state's Owner).
	//
	// THE SERVER IS THE ONLY WRITER. Nothing below is set on a client, and the client's own screen
	// draws off the replicated flag rather than off its own guess — the same "the server decides, the
	// screen reports" contract the character-select screen's header states at length. A client-side
	// team swap the server did not agree with is worse than no feature at all.
	// -----------------------------------------------------------------------------------------

	/**
	 * True while this player's team-select screen is up. Replicated from the server; the ONLY
	 * condition the screen draws on.
	 */
	UPROPERTY(Replicated)
	bool bTeamSelectOpen = false;

	/**
	 * Absolute server time (AGameStateBase::GetServerWorldTimeSeconds) at which an unanswered team
	 * select closes itself and keeps whatever team the balancer already gave this player. Zero = no
	 * deadline.
	 *
	 * A DEADLINE RATHER THAN A REMAINING DURATION, for the reason RespawnEndServerTime and
	 * CharacterSelectDeadlineServerTime both give: a client that joins late, or one whose screen is
	 * re-opened, is immediately correct instead of restarting somebody else's countdown.
	 */
	UPROPERTY(Replicated)
	float TeamSelectDeadlineServerTime = 0.f;

	bool IsTeamSelectOpen() const { return bTeamSelectOpen; }

	/** Seconds until the team select closes itself, clamped at zero. Zero when no deadline runs. */
	float GetTeamSelectTimeRemaining() const;

	/** Server only. Opens or closes the session and arms/clears the timeout. */
	void ServerSetTeamSelectOpen(bool bOpen, float DurationSeconds);

	/** H, or Trace.Teams.Select. Asks the server to put the team-select screen up. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestOpenTeamSelect();

	/** H again, or the screen's CLOSE row. Asks the server to take it down with nothing changed. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestCloseTeamSelect();

	/**
	 * THE REQUEST. Forwards to ATraceGameMode::RequestTeamChange, which owns the balance rule, and
	 * reports the verdict back through ClientTeamChangeResult. It decides nothing itself.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestTeam(ETraceTeam DesiredTeam);

	/**
	 * D31-TEAMS (b). "Switch characters while in a game, without needing to disconnect."
	 *
	 * Hands the character back and lets ATraceGameMode::PollCharacterSelect notice — the SHIPPED
	 * select screen reopens, the shipped per-team uniqueness rule runs on the pick, and the shipped
	 * lock re-closes it. That is deliberately the identical mechanism
	 * UTracePracticeRangeSubsystem::ReopenCharacterSelect uses in the practice range, rather than a
	 * second way to change character: the range's version is restricted to the range, and a second
	 * writer is exactly the drift this codebase keeps getting bitten by.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestCharacterSwitch();

	/** The verdict, back to the requesting client only. Granted is sent too — silence is not an answer. */
	UFUNCTION(Client, Reliable)
	void ClientTeamChangeResult(ETraceTeam RequestedTeam, ETraceTeamChangeResult Result);

	// ---- Last verdict, for the team-select screen's message line -------------------------------
	//
	// CLIENT-LOCAL AND NOT REPLICATED, exactly like ATracePlayerState::LastPickResult and for the
	// same reason: it is the answer to one RPC this player sent, it means nothing to anybody else,
	// and replicating it would put a refusal in front of a player who never pressed anything.

	ETraceTeamChangeResult LastTeamResult = ETraceTeamChangeResult::Granted;
	ETraceTeam LastTeamResultTeam = ETraceTeam::None;

	/** UWorld::GetTimeSeconds on the client when the verdict landed. Far in the past = none yet. */
	float LastTeamResultLocalTime = -1000.f;

	/** The possessed pawn as an ATraceCharacter, or null (unpossessed, or a non-Trace pawn). */
	ATraceCharacter* GetTraceCharacter() const;

	ATracePlayerState* GetTracePlayerState() const;

	/**
	 * Client-local world time (UWorld::GetTimeSeconds) of the last confirmed hit, or a large
	 * negative sentinel if we have never hit anything. ATraceHUD fades a marker out from this.
	 */
	float GetLastHitMarkerTime() const { return LastHitMarkerTime; }

	// -----------------------------------------------------------------------------------------
	// HUD support.
	//
	// Not part of the cross-agent contract (§7): ATraceHUD ships in the same ownership slice as
	// this controller, and these exist purely so the HUD never has to poll raw keys or guess.
	// -----------------------------------------------------------------------------------------

	/** True if the last confirmed hit was a kill — the HUD draws that marker differently. */
	bool WasLastHitMarkerAKill() const { return bLastHitMarkerWasKill; }

	/**
	 * C5 / ART_BIBLE §2.4 last row. True if the last confirmed hit was stopped by the Core carrier's
	 * shield and therefore dealt nothing.
	 *
	 * The HUD's contract (FX plan §7.4, wave 4): draw the blocked marker in shield white
	 * (0.90, 0.95, 1.00) with a different shape from the damage marker, and swap the hit sound for
	 * ShieldBlock. It is read alongside GetLastHitMarkerTime(), so it means "…about the hit that
	 * timestamp is about" and nothing else; it is cleared by the next unblocked hit.
	 */
	bool WasLastHitMarkerShieldBlocked() const { return bLastHitMarkerWasShieldBlocked; }

	/** True while the scoreboard key is held. */
	bool IsScoreboardOpen() const { return bScoreboardOpen; }

	/** Name of whoever killed us last; empty until the server tells us. */
	const FString& GetLastKillerName() const { return LastKillerName; }

	/** Cause of our last death: "Bullet", "Trail" or "Fell". NAME_None until set. */
	FName GetLastDeathCause() const { return LastDeathCause; }

	// -----------------------------------------------------------------------------------------
	// Settings
	// -----------------------------------------------------------------------------------------

	/**
	 * Rebuilds every key mapping and both look modifiers from UTraceUserSettings.
	 *
	 * Called on construction of the input data, and again on every settings change — the options
	 * screen is reachable from an in-game pause menu, so a player has to be able to fix their
	 * sensitivity and feel it on the next mouse movement, not on the next respawn.
	 *
	 * Cheap: it clears and repopulates a dozen mappings on one transient context and asks the
	 * subsystem to rebuild. Safe to call before the local player exists.
	 */
	void ApplyControlSettings();

	/**
	 * Silences every gameplay input handler and hands the mouse to the UI.
	 *
	 * Driven by ATraceHUD when the pause/settings overlay opens. Look input in particular MUST be
	 * suppressed: the overlay releases the mouse from the viewport, and the warp back to centre when
	 * capture is retaken is the same one-frame view teleport ApplyGameInputMode already defends
	 * against at spawn.
	 */
	void SetGameInputSuppressed(bool bSuppressed);

	bool IsGameInputSuppressed() const { return bGameInputSuppressed; }

	// -----------------------------------------------------------------------------------------
	// HUD data sources for mechanics owned by other slices.
	//
	// These exist so ATraceHUD never has to poll the movement component itself. They used to route
	// through a SFINAE compat header while the movement/character slices were still landing; those
	// slices have landed, so these now call the real accessors directly. A missing accessor is a
	// compile error again, which is the point — the shim's silent `false` was how a dead mechanic
	// could look exactly like a working one.
	// -----------------------------------------------------------------------------------------

	/** False when there is no living pawn to describe. */
	bool GetDashHudState(FTraceDashHudState& OutState) const;

	/** 0..1 through the 0.5s pass hold; negative when no pass is in progress. */
	float GetPassProgress() const;

	// -----------------------------------------------------------------------------------------
	// Input verification instrumentation.
	//
	// The bots call ATraceCharacter::Do* directly and never touch Enhanced Input, so before this
	// existed NOTHING in the project exercised the human input path — a broken binding would have
	// produced a completely silent, completely green run. These counters are the ground truth that
	// the synthetic-input harness in Source/Trace/Debug asserts against: they are incremented
	// inside the Enhanced Input delegates themselves, so a non-zero count is proof that the event
	// travelled OS/Slate -> viewport -> UEnhancedPlayerInput -> trigger -> our bound delegate.
	//
	// Plain ints, not UPROPERTYs: they are never replicated, never serialised and never read by
	// gameplay. Cost is a handful of bytes and one increment per event.
	// -----------------------------------------------------------------------------------------

	int32 DebugMoveEventCount = 0;
	int32 DebugLookEventCount = 0;
	int32 DebugFireStartedCount = 0;
	int32 DebugFireCompletedCount = 0;
	int32 DebugJumpCount = 0;
	int32 DebugPassCount = 0;
	int32 DebugDashCount = 0;
	int32 DebugScoreboardCount = 0;
	int32 DebugCrouchCount = 0;
	int32 DebugParryCount = 0;
	/**
	 * SPEC v26 §1. Presses that reached ATracePlayerController::DispatchCorePull and were dispatched,
	 * i.e. AFTER the per-frame de-duplication and the suppression gate.
	 *
	 * Counted where it is dispatched rather than in OnPullCoreStarted so that the shared-key tiebreak
	 * path is counted too — the question this number answers is "did the pull verb leave this
	 * machine", and it must not depend on which of the two handlers delivered it.
	 */
	int32 DebugPullPressCount = 0;
	/** Bumped by ClientNotifyHit — a server-confirmed hitscan resolution credited to us. */
	int32 DebugHitConfirmCount = 0;

	/**
	 * C5. Bumped by ClientNotifyHit when the confirmed hit was stopped by the carrier's shield.
	 * DebugHitConfirmCount still counts it — it WAS a confirmed resolve — so the two together read
	 * as "N hits, of which M did nothing", which is what the harness asserts on.
	 */
	int32 DebugShieldBlockedHitCount = 0;

	/**
	 * Spec v13 §2. Bumped on the FIRST LINE of the weapon handler, before any gate, so it counts
	 * "the key reached a bound delegate" and nothing else.
	 *
	 * IT EXISTS BECAUSE THE §2 HARNESS WAS BRIEFLY VACUOUS WITHOUT IT, and that is worth writing
	 * down. Trace.V13.Hotkeys proves a redundant press does not restart the pullout by pressing the
	 * key twice and watching the remaining time keep falling. Its first version held each synthetic
	 * key for 0.05 s and pressed again 0.05 s later, so the second press landed while the key was
	 * still logically DOWN — Enhanced Input emits Started only on a down EDGE, the handler never
	 * ran, and "the pullout was not restarted" was true because NOTHING HAPPENED. It passed. It also
	 * passed in the red arm, on the unguarded path, which is what gave it away.
	 *
	 * The fix is a longer gap, but the guarantee is this counter: the probe now asserts the count
	 * went up across every press, so a swallowed key fails the run loudly instead of passing it
	 * silently. A test that cannot fail is not evidence.
	 *
	 * ONE counter, not two: spec v15 §5 deleted the SwapWeapon handler that used to own the second,
	 * and BOTH arms of the v13 §2 harness now press the same direct-select key. That is what makes
	 * the red arm a genuine A/B — same key, same handler, same counter, one gate different.
	 */
	int32 DebugEquipPressCount = 0;

	// An accessor rather than reading the field directly, so the harness cannot write it. This
	// whole block is already inside the class's `public:` section (see line ~89) — no access
	// specifier is introduced here, because flipping one would silently re-scope every counter
	// above and the synthetic-input harness reads several of them.
	/** Read by the v13 §2 harness to prove a synthetic key actually reached a bound delegate. */
	int32 GetDebugEquipPressCount() const { return DebugEquipPressCount; }

	/**
	 * SPEC v16 §1. How many times OnReloadStarted has been ENTERED, counted on its first line before
	 * any gate — so it answers "the R key reached a bound delegate", which is a different question
	 * from "a reload started" and is the one Trace.Ammo.BindTest has to be able to ask.
	 *
	 * The distinction is the whole reason it exists, and DebugEquipPressCount's comment above records
	 * what it cost to learn: a probe that only watched the OUTCOME passed on a build where the key
	 * never reached the handler at all, because "nothing happened" and "the guard worked" look
	 * identical from outside.
	 */
	int32 DebugReloadPressCount = 0;

	int32 GetDebugReloadPressCount() const { return DebugReloadPressCount; }

	FVector2D DebugLastMoveValue = FVector2D::ZeroVector;
	FVector2D DebugLastLookValue = FVector2D::ZeroVector;

	/**
	 * Dumps every part of the Enhanced Input setup that can silently be wrong — the input
	 * component's real class, how many action bindings it carries, whether the subsystem actually
	 * accepted our mapping context, each action's ValueType, and the possession state.
	 *
	 * Lives here rather than in the harness because most of what it reports is protected state of
	 * this class. Logs at Display so it survives the default verbosity of an automated run.
	 */
	void LogInputDiagnostics(const TCHAR* Context) const;

	/**
	 * SPEC v17 §6. True when the IA_ and IMC_Trace assets were loaded, validated and adopted; false
	 * when this controller built its input in C++ (assets missing, invalid, or the toggle off).
	 *
	 * Read by Trace.Input.VerifyAssets. Not a setting — the decision is made once, in BuildInputData.
	 */
	bool IsUsingInputAssets() const { return bUsingInputAssets; }

	/**
	 * SPEC v17 §6. Prints this controller's live mapping list — action name, key and modifier
	 * classes, in order — so the asset path and the C++ path can be compared line for line.
	 *
	 * A member rather than a free function in the harness because InputMapping and every IA_* are
	 * protected, and because "what is actually bound right now" is the only question that
	 * distinguishes a migration that worked from one that merely compiled.
	 */
	void LogLiveInputMappings(const TCHAR* Context) const;

	/**
	 * SPEC v17 §6. The UInputAction each live key mapping points at, in mapping order.
	 *
	 * Exists for POINTER IDENTITY, which is the only thing that can tell the two paths apart: the
	 * C++ fallback names its NewObject'd action "IA_Move" too, so a name comparison would call the
	 * fallback a success. Trace.Input.VerifyAssets checks these against the loaded asset objects.
	 *
	 * An out-parameter of raw pointers rather than the mapping array itself, so this header does not
	 * have to pull in Enhanced Input's structs for a diagnostic.
	 */
	void GetLiveMappedActions(TArray<const UInputAction*>& OutActions) const;

	// -----------------------------------------------------------------------------------------
	// Networking diagnostics (spec v5 §0).
	//
	// Neither of these changes behaviour. They exist because the Demo 5 multiplayer report — "I'm
	// unsure if we were actually on a working network-client setup" — could not be answered from the
	// evidence that existed, and the cheapest permanent fix for that is for the answer to already be
	// in every log.
	// -----------------------------------------------------------------------------------------

	/**
	 * One Display line: net mode, whether this controller is local, whether it has authority, and
	 * the player it belongs to.
	 *
	 * Emitted for EVERY controller in BeginPlay, including the server-side proxies of remote
	 * players. On a listen server hosting one other human that is three lines, and the fact that
	 * there are three is itself the proof the connection worked.
	 */
	void LogNetworkRole(const TCHAR* Context) const;

	/**
	 * `TraceNetInfo` in the console: role, endpoint, every local adapter address, and the full
	 * PlayerState roster with bots marked.
	 *
	 * Exec rather than a cvar because the question it answers ("who is actually in this match?") is
	 * asked once, by hand, when something looks wrong — and because a cvar cannot print a list.
	 */
	UFUNCTION(Exec)
	void TraceNetInfo();

private:
	/**
	 * Emits ONE Display line the first time each action ever fires, then never again.
	 *
	 * This is not covered by Trace.LogInput, and deliberately so. The next time somebody reports
	 * "I couldn't shoot", the answer to "did the key even reach the game?" has to be in the log
	 * they already have — not behind a cvar nobody thought to set before playing. Seven lines per
	 * session is nothing; the absence of the "Fire" line is a diagnosis.
	 */
	void LogFirstEventOfAction(uint8 Bit, const TCHAR* ActionName);

	/** One bit per action, so the line above is emitted exactly once each. */
	uint8 FirstEventLoggedMask = 0;

	static constexpr uint8 FirstEvent_Move       = 1 << 0;
	static constexpr uint8 FirstEvent_Look       = 1 << 1;
	static constexpr uint8 FirstEvent_Jump       = 1 << 2;
	static constexpr uint8 FirstEvent_Fire       = 1 << 3;
	static constexpr uint8 FirstEvent_Pass       = 1 << 4;
	static constexpr uint8 FirstEvent_Dash       = 1 << 5;
	static constexpr uint8 FirstEvent_Scoreboard = 1 << 6;
	static constexpr uint8 FirstEvent_Crouch     = 1 << 7;
	// Parry deliberately has no bit: the mask is a uint8 and is full. Parry is the newest action, and
	// unlike the others it has a loud diagnosis of its own — Trace.DebugParry reports the refusal
	// reason for every attempt — so it is the one action whose "did my key do anything?" question is
	// already answerable without widening the mask.

public:

protected:
	// ---- Enhanced Input data ------------------------------------------------------------------
	//
	// Populated EITHER from /Game/Trace/Input (preferred) OR by NewObject (the live fallback) —
	// see the file header for the whole argument. Which one ran is in the log and in
	// IsUsingInputAssets().
	//
	// These MUST stay UPROPERTYs. On the C++ path nothing else roots the NewObject'd actions; on
	// the asset path the pointers are what keep the loaded assets referenced. Transient because
	// they are resolved from scratch on every load and must never be serialised.

	/**
	 * Always a PER-CONTROLLER object, never the asset itself: on the asset path this is a
	 * DuplicateObject of the loaded IMC_Trace. ApplyControlSettings calls UnmapAll/MapKey on it a
	 * dozen times per settings change, and doing that to a loaded .uasset would dirty the asset.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> InputMapping;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Move;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Look;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Jump;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Fire;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Pass;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Dash;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Scoreboard;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Crouch;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Parry;

	/**
	 * Spec v13 §2. DIRECT SELECT, default 1 and 2. These two are the WHOLE weapon input model as of
	 * spec v15 §5 — there is no IA_SwapWeapon any more, and there is deliberately no IA_Swing
	 * either: the swing rides IA_Fire, because UTraceWeaponComponent's fire path branches on
	 * IsKnifeEquipped(), so mouse1 shoots with the gun and swings with the knife.
	 *
	 * Two actions rather than one action with a value: an Enhanced Input Boolean action carries no
	 * payload that could say WHICH weapon, and giving them one action plus a modifier would put the
	 * weapon identity inside the mapping, where the rebind screen could not see it and where a
	 * player rebinding "equip" to one key would silently get half a mechanic.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_EquipKnife;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_EquipGun;

	/**
	 * *** SPEC v29 §5 — THE THIRD WEAPON BIND. "Pressing 2 pulls out pistol, 3 pulls out smg." ***
	 *
	 * A THIRD ACTION, not a modifier on IA_EquipGun, for the reason the two above already give: an
	 * Enhanced Input Boolean action carries no payload that could say WHICH weapon, so a shared
	 * action would put the weapon identity inside the MAPPING, where the rebind screen cannot see it.
	 * The keybind page lists ACTIONS; an SMG that is only a second key on the pistol's action is an
	 * SMG the player cannot rebind on its own.
	 *
	 * Bound on Started only, exactly like its two neighbours: there is no held state to release.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_EquipSmg;

	/**
	 * SPEC v14 §5 — the two ability binds, default E and V.
	 *
	 * IA_Ability is bound on Started only: the activated ability is a press. IA_AbilitySecondary is
	 * bound on Started AND Completed AND Canceled, because Mace's suspend is a HOLD and a dropped
	 * release edge would leave her floating — the same asymmetry, and for the same reason, as
	 * IA_Pass's hover pass.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Ability;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_AbilitySecondary;

	/**
	 * SPEC v16 §1 — "R to reload". Default R, rebindable like everything else.
	 *
	 * Bound on Started ONLY, with the same asymmetry argument as IA_EquipKnife / IA_EquipGun: there is
	 * no held state to release, and a Completed binding would send a second reload request on key-up
	 * that UTraceWeaponComponent::RequestReload would then swallow because a reload is already
	 * running — a binding that only works because something downstream ignores it.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Reload;

	/**
	 * SPEC v26 §1 — "Make parry and pull core two separate binds in the settings menu." Default F.
	 *
	 * Its own UInputAction and not a second mapping on IA_Parry, which is the whole point of the item:
	 * two actions is what makes two rows on the keybind page, two rebindable keys, and two independent
	 * handlers. One action with two keys would be one bind wearing a second key.
	 *
	 * Bound on Started AND Completed AND Canceled, because the pull is a HOLD — "releasing right mouse
	 * cancels" was spec v25 §2's rule and it is unchanged by moving the verb to its own key. A dropped
	 * release edge here leaves a pull ring filling on the server behind a pause menu, which is the
	 * exact failure OnPassCompleted's comment documents.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_PullCore;

	/**
	 * *** SPEC v28 §10 — THE MELEE BIND. Default RIGHT MOUSE, rebindable like everything else. ***
	 *
	 * The whole verb is TraceMelee::HandleMeleeInput and this action does nothing but deliver edges to
	 * it. In particular the PRECEDENCE — "if and only if a player is looking at a pullable core, and
	 * being shown the circle icon to pull, this keybind should override the melee keybind" — is not
	 * decided here: HandleMeleeInput asks ATraceCore::CanPullNow, which is the same predicate ATraceHUD
	 * asks to decide whether to DRAW the circle. One fact, not two that can drift.
	 *
	 * Bound on Started AND Completed AND Canceled, and BOTH edges are load-bearing. The swing is a
	 * press-edge action and does not care, but the press may have gone to the Core PULL, which is a
	 * HOLD — a dropped release leaves a pull ring filling on the server behind a pause menu or a lost
	 * window focus. HandleMeleeInput forwards every release to the Core unconditionally so the caller
	 * never has to remember which verb the press went to; see its comment.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Melee;

	/**
	 * *** SPEC v31 §5 — THE KNIFE FLOURISH. Default F, rebindable like everything else. ***
	 *
	 * "Inspect (3.20 s) is a flourish — bind it to F, as a new rebindable action in the settings page
	 * like every other action." The verb is TraceKnifeView::RequestInspect and this action does
	 * nothing but deliver the press to it.
	 *
	 * *** PRESS ONLY. There is deliberately no Completed/Canceled binding, and that is a decision. ***
	 * Every other action in this file that binds the release does so because it owns HELD state that
	 * a dropped release would strand: the Core pull's ring keeps filling, the parry's shared-key
	 * tiebreak keeps a hold latched, fire keeps firing. The flourish owns none — it is a one-shot
	 * animation of a fixed 3.20 s that ends because the SEQUENCE ends, and there is nothing a release
	 * could cancel that a real action does not already outrank. Binding a release here would be a
	 * handler that exists to do nothing, which is worse than no handler.
	 *
	 * ITS KEY WAS TAKEN FROM ETraceInputAction::PullCore, which moved to G. That is written up in
	 * full on Default_PullCore and on the enumerator; it is mentioned here because a reader of this
	 * header hunting "why is my F not pulling any more" should not have to find it.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Inspect;

	/**
	 * SPEC v31 §5. The F handler: one call, no decision. Every refusal lives at the verb.
	 *
	 * Suppressed while a menu owns input, like every other press in this file.
	 */
	void OnInspectStarted();

	/**
	 * Resolves InputMapping and every IA_* exactly once, then lays down the key mappings.
	 *
	 * The two halves are separate on purpose. The ACTIONS must be resolved exactly once and never
	 * replaced, because SetupInputComponent binds delegates to those specific objects and a fresh
	 * UInputAction would leave every binding pointing at an action nothing maps to. The KEY MAPPINGS
	 * are torn down and rebuilt on every settings change. Cheap and safe to call repeatedly.
	 *
	 * SPEC v17 §6: tries the assets first, falls back to the C++ constructors. Both are below.
	 */
	void BuildInputData();

	/**
	 * SPEC v17 §6. Loads /Game/Trace/Input, validates it against the C++ table and adopts it.
	 *
	 * @return true only if EVERY action loaded, every ValueType and AccumulationBehavior matched
	 *         what the C++ path would have produced, and the mapping context loaded. Partial
	 *         success is treated as failure and nothing is assigned — half an input scheme is worse
	 *         than the fallback, and the fallback is known-good.
	 *
	 * Never mutates a loaded asset. Validation is a comparison, not a repair: an asset that
	 * disagrees with the C++ table is a REGENERATE that did not happen, and silently fixing it up
	 * in memory would hide exactly the drift Trace.Input.VerifyAssets exists to find.
	 */
	bool TryAdoptInputAssets();

	/** The original path, unchanged: NewObject for the context and all fifteen actions. */
	void ConstructInputDataInCode();

	/** Registers InputMapping with the local player's Enhanced Input subsystem. Idempotent. */
	void AddInputMappings();

	/** Unregisters it again. Called from EndPlay so travel does not leak stale contexts. */
	void RemoveInputMappings();

	/**
	 * Puts the viewport into game-only input with the initial mouse-down NOT consumed, so the click
	 * that recaptures a window also registers as a shot. Re-applied on every possession; see the
	 * implementation for why the default FInputModeGameOnly is wrong for a shooter.
	 */
	void ApplyGameInputMode();

	/**
	 * UI PLAN WP2.4 — pushes the player's stored CALL SIGN at the server, once, on the owning client.
	 *
	 * Until this existed no human ever got a name: only bots called SetPlayerName, so every human
	 * scoreboard row read `Mac-3249D6BCCE489DF8`. It goes through the engine's own rename path
	 * (APlayerController::ServerChangeName -> AGameModeBase::ChangeName -> the replicated
	 * APlayerState::SetPlayerName), NOT a local SetPlayerName, because the point is that the name
	 * reaches every machine and not just the one that typed it.
	 *
	 * RETRIES, because PlayerState is null in BeginPlay on a client that has not been welcomed yet —
	 * exactly the machine the whole feature is about. A no-op when the stored name already matches,
	 * so a respawn or a travel does not send a rename per pawn.
	 */
	void ApplyStoredCallSign(int32 AttemptsLeft);

	// ---- Input handlers -----------------------------------------------------------------------
	void OnMoveInput(const FInputActionValue& Value);
	void OnLookInput(const FInputActionValue& Value);
	void OnJumpStarted();
	void OnJumpCompleted();
	void OnFireStarted();
	void OnFireCompleted();
	void OnPassStarted();
	/** Release edge of the held hover pass (spec §4). Must fire even when input is suppressed. */
	void OnPassCompleted();
	void OnDashStarted();
	/** Press edge of the parry (spec v3 §3). Carrier-only; the refusal lives in TraceParry. */
	void OnParryStarted();
	/**
	 * Release edge. Parry is a tap, not a hold, so this does nothing — but it is bound anyway, and
	 * Canceled with it, so every button in this class has the same Started/Completed/Canceled shape.
	 * That symmetry is what fixed the pass-input latch; an action bound on Started alone is how a
	 * held-key bug gets reintroduced the day somebody makes the window hold-to-extend.
	 */
	void OnParryCompleted();

	/**
	 * SPEC v26 §1. Press and release edges of the CORE PULL, which is now its own bind (default F).
	 *
	 * Makes no decision of its own, exactly like OnParryStarted: every refusal (not a turnover, wrong
	 * team, not hovering, no line of sight, already carrying, window expired) lives behind
	 * ATraceCore's own authoritative gate. An input handler that second-guessed any of those would be
	 * a client deciding the outcome of a server-authoritative verb.
	 *
	 * The RELEASE is deliberately not gated on bGameInputSuppressed and uses GetPawn rather than
	 * GetLivingCharacter — see OnParryCompleted for the argument. Opening the pause menu mid-pull
	 * suppresses input; dying mid-pull makes the pawn non-living. Those are the two cases where the
	 * cancel must still be delivered.
	 */
	void OnPullCoreStarted();
	void OnPullCoreCompleted();

	/**
	 * SPEC v28 §10 — press and release edges of the MELEE bind (right mouse by default).
	 *
	 * Makes NO decision of its own, exactly like OnParryStarted and OnPullCoreStarted: the swing's
	 * refusals (dead, carrying, mid-pullout, dashing, on cooldown) live behind
	 * UTraceWeaponComponent::CanSwing and the pull's live behind ATraceCore, and the choice between
	 * the two is TraceMelee::ShouldCorePullOverrideMelee. A second opinion here is how a predicted
	 * state and an authoritative one come to disagree.
	 *
	 * The RELEASE is deliberately NOT gated on bGameInputSuppressed and uses GetPawn rather than
	 * GetLivingCharacter, for the reason OnParryCompleted and OnPullCoreCompleted both document:
	 * opening the pause menu mid-pull suppresses input and dying mid-pull makes the pawn non-living,
	 * and those are exactly the two cases where the cancel MUST still be delivered.
	 */
	void OnMeleeStarted();
	void OnMeleeCompleted();

	/**
	 * SPEC v26 §1. The one place the pull is dispatched, and the reason it is a function.
	 *
	 * Two handlers can reach the pull on one frame — its own bind, and (only when the player has put
	 * both actions on the SAME key) the parry's tiebreak path. This collapses that into exactly one
	 * dispatch per frame per edge, so a deliberately shared key sends one RPC rather than two.
	 *
	 * @param bPressed  true for the press edge, false for the release.
	 */
	void DispatchCorePull(bool bPressed);

	/** True when the player has deliberately bound PARRY and PULL CORE to the same key. */
	bool DoParryAndPullShareAKey() const;

	/** Frame number of the last DispatchCorePull, per edge, so the two callers cannot double-fire. */
	uint64 LastPullPressFrame = 0;
	uint64 LastPullReleaseFrame = 0;

	/**
	 * Spec v13 §2, as rewritten by SPEC v29 §5. Press edge of the THREE DIRECT-SELECT weapon binds.
	 * Press-only, and that asymmetry is deliberate: there is no held state here, so a release binding
	 * would fire a second request on key-up.
	 *
	 *     3  OnEquipKnifeStarted  ETraceEquippedWeapon::Knife  the KNIFE, +22% speed, 0.65x pullout
	 *     1  OnEquipGunStarted    ETraceEquippedWeapon::Gun     the pistol
	 *     2  OnEquipSmgStarted    ETraceEquippedWeapon::Smg     the SMG
	 *
	 * *** SPEC v31 §1 RE-KEYED ALL THREE AND DELETED THE STOW STATE. *** Verbatim: "1 is pistol, 2 is
	 * smg, 3 is knife". The digits above are the SHIPPED DEFAULTS and nothing more — each is read live
	 * from TraceInputActions::Info(), all three are rebindable, and all three ConfigIds were migrated
	 * ("StowGuns" -> "KnifeSlot", "EquipPistol" -> "PistolSlot", "EquipSmg" -> "SmgSlot") because
	 * every key changed MEANING, not just position. The knife handler no longer stows anything: with
	 * bDualWieldKnife OFF it selects the blade as a weapon, which is what it did before v24.
	 *
	 * *** THE PARAMETER IS THE WEAPON, NOT A BOOL, AND THAT IS THE WHOLE v29 §5 CHANGE HERE. *** It
	 * used to be `bool bWantKnife`, which could only ever name two of three states — so v28 §10 had
	 * to bolt a remap onto TraceMelee::RequestEquipIfDifferent to turn "knife" into "pistol" behind
	 * this function's back. Passing the weapon deletes that remap and leaves exactly ONE translation
	 * from a key to a weapon in the game: these three handlers.
	 *
	 * The two handler names keep their v13 spellings because ETraceInputAction does (see that enum's
	 * comment — renaming it would break another slice's file this pass). OnEquipKnifeStarted is the
	 * KNIFE key: v29 §5 made it the stow key and v31 §1 gave it back to the blade, so the spelling is
	 * once again the plain truth rather than a historical accident.
	 *
	 * ONE implementation behind three handlers rather than three copies: the only difference between
	 * them is the destination weapon, and the idempotence guard is the part that must not be allowed
	 * to drift between them.
	 */
	void OnEquipKnifeStarted();
	void OnEquipGunStarted();
	void OnEquipSmgStarted();
	void HandleDirectEquip(ETraceEquippedWeapon Desired, const TCHAR* ActionLabel);

	/**
	 * SPEC v16 §1. Press edge of the reload bind (R).
	 *
	 * Makes NO decision of its own — every refusal (full clip, already reloading, dead, carrying, the
	 * knife out, an ability-loaded clip) lives behind UTraceWeaponComponent::RequestReload, which is
	 * also what the AUTOMATIC reload goes through. A second opinion here about whether a reload is
	 * legal is exactly how a predicted state and an authoritative one come to disagree, and it is the
	 * same reasoning OnParryStarted's comment gives.
	 */
	void OnReloadStarted();

	void OnCrouchStarted();
	void OnCrouchCompleted();
	void OnScoreboardStarted();
	void OnScoreboardCompleted();

	/**
	 * SPEC v14 §5. Press edge of the activated ability (E).
	 *
	 * Routed through UTraceAbilityInputRelay::RouteActivatePressed rather than straight into
	 * UTraceAbilityComponent::HandleActivatePressed, because ONE character's press is not an
	 * activation: Mace's second press of Spike is the reactivation that pulls her, and
	 * TryActivate() correctly refuses it (the throw's own 20 s cooldown is running). That routing
	 * rule is written down exactly once, in the relay, and both the key path and the relay's own
	 * interim poll call it.
	 */
	void OnAbilityStarted();

	/** SPEC v14 §5. Press / release of the secondary ability (V) — Mace's suspend is a hold. */
	void OnAbilitySecondaryStarted();
	void OnAbilitySecondaryCompleted();

	/** GetTraceCharacter(), but null when the pawn is missing or dead. */
	ATraceCharacter* GetLivingCharacter() const;

private:
	/**
	 * SPEC v18 §1c. Re-fires the press edge of every HOLD-shaped gameplay action whose key is still
	 * physically down at the moment gameplay input is handed back.
	 *
	 * Called from exactly one place, the restore branch of SetGameInputSuppressed, and it is the
	 * mirror of the DoFireReleased/StopJumping/DoMove(0) that the suppress branch does. Enhanced Input
	 * fires Started on a TRANSITION, so a key that was already held when a menu opened produces no
	 * further Started after it closes — the player has to release and press again. Held axes recover
	 * on their own (IA_Move is bound on Triggered); buttons are what get lost, which is why §1c reads
	 * as "movement is fine but the action did not happen".
	 *
	 * NOT A BUFFER. It asks the input device what is down RIGHT NOW, so a press made and released
	 * under the overlay is correctly gone. And deliberately only the hold-shaped actions — see the
	 * comment at the call site for why a resting finger must not spend a dash or a 35 s ability.
	 */
	void RedeliverHeldPressEdges();

	/** Priority of our mapping context. Nothing else adds a context, so 0 is fine. */
	static constexpr int32 InputMappingPriority = 0;

	/** Minimum seconds between honoured ServerRequestRespawn calls (anti-spam). */
	static constexpr float RespawnRequestCooldown = 0.5f;

	/** How long look input is dropped after the viewport takes the mouse. See ApplyGameInputMode. */
	static constexpr float LookSuppressAfterCapture = 0.5f;

	/** Look rate budget. A look event above this rate is discarded as a capture artefact. */
	static constexpr float MaxLookDegreesPerSecond = 2500.f;

	/** Floor for the rate budget, so a very high frame rate cannot make the threshold absurdly tight. */
	static constexpr float MinLookSpikeDegrees = 20.f;

	/** World time before which look input is discarded. */
	float IgnoreLookUntilTime = 0.f;

	/** Far enough in the past that the HUD's "age" test fails on the first frame. */
	static constexpr float NeverHitSentinel = -1000.f;

	float LastHitMarkerTime = NeverHitSentinel;
	bool bLastHitMarkerWasKill = false;
	/** C5. Written by every ClientNotifyHit, so it always describes the hit LastHitMarkerTime dates. */
	bool bLastHitMarkerWasShieldBlocked = false;
	ETraceHitZone LastHitMarkerZone = ETraceHitZone::None;

	bool bScoreboardOpen = false;

	/** SPEC v17 §6. Set once by BuildInputData. See IsUsingInputAssets(). */
	bool bUsingInputAssets = false;

	/**
	 * Latches once AddInputMappings() has REPORTED a mapping-context failure, so a persistent
	 * misconfiguration logs once instead of once per respawn. The check itself is not latched — a
	 * failure that only appears later (seamless travel, a second local player) still gets reported.
	 */
	bool bInputFailureReported = false;

	FString LastKillerName;
	FName LastDeathCause = NAME_None;

	/** Spec v6 §3, remote-carrier feedback. Written only by ClientNotifyParryKill. */
	float LastParryKillTime = NeverHitSentinel;
	FString LastParryKillVictim;

	/** True while the pause/settings overlay owns the screen. See SetGameInputSuppressed. */
	bool bGameInputSuppressed = false;

	/** Registration with UTraceUserSettings::OnChanged, released in EndPlay. */
	FDelegateHandle SettingsChangedHandle;

	/** WP2.4. Retry timer for the call-sign push while the client is still waiting for a PlayerState. */
	FTimerHandle CallSignRetryHandle;

	/** Server-side: world time of the last respawn request we acted on. */
	float LastRespawnRequestTime = NeverHitSentinel;

	// ---- D31-TEAMS ---------------------------------------------------------------------------

	/**
	 * Server-side: closes an unanswered team select. A ONE-SHOT TIMER rather than a tick, because
	 * this class does not otherwise tick on the server and adding one for a countdown that fires at
	 * most once per player per opening would be a per-frame cost for a per-minute event.
	 *
	 * Cleared whenever the session closes for any other reason, so a player who picks at 14.9 s
	 * cannot have their screen "closed" again half a second later.
	 */
	FTimerHandle TeamSelectTimeoutHandle;

	/** Server-side: world time of the last team-select request we acted on. Anti-spam, like respawn. */
	float LastTeamRequestTime = NeverHitSentinel;

	/** Minimum seconds between honoured team-select requests from one client. */
	static constexpr float TeamRequestCooldown = 0.25f;

	/** Fired by TeamSelectTimeoutHandle. Closes the session and keeps the team the balancer gave. */
	void CloseTeamSelectOnTimeout();

	// -----------------------------------------------------------------------------------------
	// VIEW KICK (FX_AUDIO_PLAN §1.5) — three numbers and a start time, all local to this machine.
	// -----------------------------------------------------------------------------------------

	/** Peak degrees of the running kick. 0 = nothing running, which is the common case. */
	float ViewKickAmplitudeDeg = 0.f;

	/** Total length of the running kick, in seconds. */
	float ViewKickDurationSeconds = 0.f;

	/** Oscillations per second of the running kick. */
	float ViewKickNoiseHz = 0.f;

	/**
	 * UNPAUSED WORLD TIME the kick started at.
	 *
	 * World time rather than FPlatformTime so a paused or dilated world holds the kick still instead
	 * of running it out behind a menu — the same clock choice the rest of this file's timers make.
	 */
	float ViewKickStartTime = NeverHitSentinel;

	/** The camera-manager modifier that applies the offset. Created lazily on the first kick. */
	UPROPERTY(Transient)
	TObjectPtr<class UTraceViewKickModifier> ViewKickModifier = nullptr;
};

/**
 * FX_AUDIO_PLAN §1.5 — the one-line camera modifier the kick rides on.
 *
 * WHY A MODIFIER AND NOT A CONTROL-ROTATION OFFSET: the camera manager applies modifiers to the POV
 * *after* the view target has produced it (APlayerCameraManager::DoUpdateCamera ->
 * ApplyCameraModifiers), so this moves the rendered view and nothing else. Writing the same offset
 * into the control rotation would move the crosshair, the aim ray and the server's validation with
 * it — which is gun recoil, a mechanic spec v25 §5 deliberately removed.
 *
 * It holds no state: it asks its owning controller for the offset each frame, so a kick that is
 * cancelled, replaced or expired needs no bookkeeping here.
 */
UCLASS(NotBlueprintable)
class TRACE_API UTraceViewKickModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	virtual bool ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV) override;
};
