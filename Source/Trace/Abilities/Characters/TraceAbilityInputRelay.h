// Trace — the interim input path for the two ability keys (spec v14 §5).
//
// ===================================================================================================
// WHY THIS FILE EXISTS, AND WHAT MUST DELETE IT
// ===================================================================================================
//
// Spec v14 §5: "Activated abilities bind to E by default, rebindable" and "Mace's suspend needs its
// own bind (V, per the doc)."
//
// Neither bind exists. ETraceInputAction (Settings/TraceUserSettings.h) has no ability entry and
// ATracePlayerController binds no ability action — both foundation reports flagged this as a
// cross-file need that nobody owns yet, and the character files are not allowed to edit either file.
// Without a key, Mace's suspend is unreachable by any means other than a console command, and an
// ability that can only be fired from the console is a stub, not a working foundation.
//
// So this component polls the two keys itself, on the owning client, and relays them to the server.
// It is DELIBERATELY the least clever thing that produces a playable ability:
//
//   * It asks UTraceUserSettings for a binding whose ConfigId is "Ability" / "AbilitySecondary"
//     FIRST, so the moment somebody appends those two actions to ETraceInputAction this component
//     starts honouring the player's rebind with no change here.
//   * Only when that lookup finds nothing does it fall back to the raw E and V keys the doc names.
//   * Every press is routed into the framework's own entry points — UTraceAbilityComponent::
//     HandleActivatePressed() and HandleSecondaryPressed()/Released() — never into a private path.
//     So when ATracePlayerController does bind the actions, both paths call the same functions and
//     the duplicate press is absorbed by the cooldown, which is already idempotent.
//
// DELETE THIS COMPONENT the day ETraceInputAction gains the two actions and ATracePlayerController
// binds them. Nothing else depends on it: it holds no gameplay state.
//
// ---------------------------------------------------------------------------------------------
// THE ONE THING IT DOES THAT A KEY BIND WOULD NOT: MACE'S REACTIVATION
// ---------------------------------------------------------------------------------------------
//
// Spec v14 §6, Mace: "Reactivating pulls her toward it at the momentum ceiling."
//
// UTraceAbilityComponent::TryActivate() refuses while the activated cooldown is running, and it does
// so BEFORE it asks the character's CanActivate(). That check is correct and I am not going to
// weaken the one cooldown contract in the framework for one character. But it does mean the second
// press of Spike — the reactivation — can never reach Mace through TryActivate(), because the throw
// has just started a 20 s cooldown.
//
// So the relay asks Mace first: if she has a spike embedded and ready, the press is a REACTIVATION
// and goes straight to UTraceAbilitySetMace::RequestSpikePull() (a server RPC on this component).
// Otherwise it is a normal activation and goes to HandleActivatePressed() like everybody else's.
// The reactivation is free, which is the doc's reading — it is the second half of one cast, not a
// second cast — and the HUD's cooldown therefore stays truthful for the whole 20 s.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "InputCoreTypes.h"          // FKey
#include "UObject/ObjectMacros.h"

#include "TraceAbilityInputRelay.generated.h"

class APlayerState;

/**
 * Polls the two ability keys on the owning client and relays them to the server.
 *
 * Lives on the APlayerState, next to UTraceAbilityComponent, for the same two reasons that component
 * does: the PlayerState's owner is the controller (so a client -> server RPC is legal), and it is not
 * destroyed on every respawn the way the pawn is.
 */
UCLASS(ClassGroup = (Trace), meta = (BlueprintSpawnableComponent))
class TRACE_API UTraceAbilityInputRelay : public UActorComponent
{
	GENERATED_BODY()

public:
	UTraceAbilityInputRelay();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * SERVER ONLY, idempotent. Attaches a relay to @p ForPlayerState if it has none.
	 *
	 * Called from Mace's and Oyster's OnEquipped(). Deliberately not from the world subsystem: a
	 * player with no character needs no relay, and a relay attached to all ten PlayerStates would be
	 * ten components polling nothing.
	 */
	static UTraceAbilityInputRelay* EnsureOn(APlayerState* ForPlayerState);

	/** The relay on @p ForPlayerState, or null. */
	static UTraceAbilityInputRelay* Find(const APlayerState* ForPlayerState);

	/** The key the ACTIVATED ability is on: the player's "Ability" bind if it exists, else E. */
	static FKey ResolveActivateKey();

	/** The key the SECONDARY ability is on: the player's "AbilitySecondary" bind if it exists, else V. */
	static FKey ResolveSecondaryKey();

	/** True when the two actions have been appended to ETraceInputAction and this relay's POLL is redundant. */
	static bool AreRealBindsInstalled();

	// ---------------------------------------------------------------------------------------------
	// THE ROUTERS — one definition of "what a press of E / V means", called by BOTH input paths
	// ---------------------------------------------------------------------------------------------
	//
	// Spec v14 §5's binds now exist (ETraceInputAction::Ability / AbilitySecondary) and
	// ATracePlayerController binds them, so the poll above is switched off — but the relay is NOT
	// deleted, because one of its two jobs was never about the missing bind:
	//
	//   * MACE'S REACTIVATION. See the block at the top of this file. TryActivate() refuses while the
	//     cooldown the throw just started is running and checks that BEFORE CanActivate(), so the
	//     second press of Spike cannot reach her through it. A key bind does not change that; the
	//     routing rule has to live somewhere, and it lives here, once.
	//   * The Server RPC that carries that reactivation.
	//
	// Both routers are safe with no relay component present (every character except Mace and Oyster
	// has none) and safe on a dedicated server, a listen-server host and a remote client.

	/** E pressed: Mace's reactivation if she has a spike ready, otherwise the ordinary activation. */
	static void RouteActivatePressed(APlayerState* ForPlayerState);

	/** V pressed or released. Predicts locally and sends the authoritative half. */
	static void RouteSecondaryEdge(APlayerState* ForPlayerState, bool bDown);

	// ---------------------------------------------------------------------------------------------
	// Client -> server
	// ---------------------------------------------------------------------------------------------

	/** V went down or up. Reliable because a dropped "released" would leave Mace floating. */
	UFUNCTION(Server, Reliable)
	void ServerSetSecondaryHeld(bool bHeld);

	/** E pressed while Mace has a spike embedded: the reactivation, which TryActivate() cannot carry. */
	UFUNCTION(Server, Reliable)
	void ServerRequestSpikePull();

private:
	/** Rising/falling edge state for the local poll. Owning client only; meaningless elsewhere. */
	bool bPolledActivateDown = false;
	bool bPolledSecondaryDown = false;

	/** Says "the poll is off" once per relay rather than once per frame. */
	bool bLoggedPollDisabled = false;

	/** One press handler, shared by the local poll so the listen-server host and a client agree. */
	void HandleActivateEdge(bool bDown);
	void HandleSecondaryEdge(bool bDown);

	/** Looks up a binding by its stable ConfigId. Returns an invalid key when the action does not exist. */
	static FKey FindBindingByConfigId(const TCHAR* ConfigId);
};
