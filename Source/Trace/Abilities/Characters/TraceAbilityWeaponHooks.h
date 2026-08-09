// Trace — the weapon's one call into the ability layer (spec v14 §6).
//
// ===================================================================================================
// WHY THIS TINY HEADER EXISTS AT ALL
// ===================================================================================================
//
// X's activated ability is "Sting": "loads the 5 bees into his gun… His NEXT FIVE BULLETS apply
// vulnerable on hit". That is the only place in spec §6 where a hitscan bullet has to tell an ability
// that it landed, and UTraceAbilityComponent has no NotifyBulletHit() — the framework's notification
// list (NotifyDash*, NotifyPawnSpawned/Died, NotifyKill) was written before Sting existed.
//
// The alternative was for Gameplay/TraceWeaponComponent.cpp to `#include "TraceAbilitySetX.h"` and
// Cast<UTraceAbilitySetX>. That works, and it is wrong: the gun would then know the name of one
// character, and the second character that needs a bullet hook would add a second cast next to the
// first. This header is the seam instead — the weapon calls one character-agnostic function, and the
// dispatch to whichever ability sets care lives in the ability layer where it belongs.
//
// WHEN THE FRAMEWORK GROWS A REAL HOOK (a `NotifyBulletHit` on UTraceAbilityComponent, mirroring
// NotifyDashHitCharacter), this file becomes a one-line forwarder and then goes away. It is
// deliberately the smallest possible surface so that migration is a single edit.

#pragma once

#include "CoreMinimal.h"

class ATraceCharacter;

namespace TraceAbilityWeaponHooks
{
	/**
	 * SERVER ONLY. A hitscan bullet fired by @p Shooter landed on @p Victim, and the damage has
	 * already been applied.
	 *
	 * Called from UTraceWeaponComponent::ServerFire_Implementation, once per bullet that actually
	 * found a character. Safe with nulls, safe when the shooter has no character, and a no-op for
	 * every character except X with bees loaded.
	 *
	 * *** IT IS CALLED AFTER THE DAMAGE, NOT BEFORE. *** Spec §6 says Sting's bullets apply
	 * vulnerable "at NORMAL damage" — so the bullet that delivers the mark must not also be amplified
	 * by it. Applying the mark after UTraceHealthComponent::ApplyDamage has returned is what makes
	 * that true by construction rather than by a comment asking the next reader to be careful.
	 */
	TRACE_API void OnBulletHit(ATraceCharacter* Shooter, ATraceCharacter* Victim, bool bHeadshot);
}
