// Trace — applying the cloak look to a pawn, from more than one ability.
//
// WHY A SHARED ENTRY POINT AND NOT A SECOND COPY. Elle's SHIMMER has cloaked a pawn since spec v18,
// and the code that does it — dimming every colour parameter, killing every emissive, dropping the
// shadow, and restoring through the same ApplyTeamColors() path everything else comes back through —
// is careful, material-specific and easy to get subtly wrong. Demo 35 added a SECOND ability that
// cloaks (Oyster's dash cloak), and two copies of that would drift the first time a material changed.
//
// SO THE IMPLEMENTATION DID NOT MOVE. It is still where it was written, beside Elle's own cloak
// state and the FX sweep that belongs to her alone; this is one function that reaches it. Moving the
// whole thing into a shared file would have been the tidier diagram and a worse trade: it would edit
// a shipped, verified ability to serve a new one.
//
// WHAT THIS IS NOT: it is not cloak STATE. How long a cloak lasts, what starts it and what cancels
// it belong to the ability — Elle's runs off a Core pass, Oyster's off a jump after a dash — and
// they keep their own timers in their own replicated structs. This is only the look.

#pragma once

#include "CoreMinimal.h"

class ATraceCharacter;

namespace TraceAbilityCloak
{
	/**
	 * Turns the cloak look on or off for @p Pawn.
	 *
	 * Cosmetic and idempotent: safe to call every frame with the same value, and safe on a dedicated
	 * server (where it does nothing). Restoring goes through the pawn's normal team-colour path, so
	 * it needs no memory of what the materials looked like before.
	 */
	TRACE_API void Apply(ATraceCharacter* Pawn, bool bCloakOn);
}
