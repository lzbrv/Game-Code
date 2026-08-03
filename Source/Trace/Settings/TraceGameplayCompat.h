// Trace — compile-time feature detection for gameplay APIs that land in a different ownership slice.
//
// WHAT PROBLEM THIS SOLVES
// The settings/HUD slice has to ship UI for three mechanics that the movement and character slices
// are adding in the same pass: dash CHARGES (spec §5 — the carrier gets two), the BOOST cooldown
// (§5), and the 0.5s hold that completes a PASS (§4). Those accessors do not exist yet in the
// headers this slice compiles against.
//
// The two obvious options are both bad. Calling them anyway does not build, so the HUD could not be
// written at all until someone else landed first. Hard-coding placeholders means the real data has
// to be wired up later by hand, which is precisely the kind of follow-up that gets forgotten and
// ships as a permanently fake meter.
//
// So: detect them. Each helper below compiles to a direct call when the member exists and to a
// stated fallback when it does not, with no runtime cost either way and no build break in either
// direction. ATraceHUD logs exactly which affordances came up live on its first draw, so "the boost
// meter is missing" is one grep away from an answer instead of a bisect.
//
// THESE ARE SCAFFOLDING. Once the movement and character slices have landed, every helper here
// should be collapsing to its detected branch. If one is still reporting `false` at that point, the
// signature it is looking for does not match what was actually written — see the report for the
// exact expected declarations.

#pragma once

#include "CoreMinimal.h"

#include <type_traits>
#include <utility>

namespace TraceCompat
{
	/**
	 * Declares a detector `Name<T>::value` that is true when `T` has a callable member `Call`.
	 *
	 * The partial specialisation only matches when the expression is well-formed, because the
	 * substitution failure removes it from the overload set (SFINAE). `decltype(void(...))` collapses
	 * whatever the member returns to `void`, so it matches the primary template's default argument.
	 */
	#define TRACE_DEFINE_MEMBER_DETECTOR(Name, Call)                                                  \
		template <typename T, typename = void>                                                        \
		struct Name : std::false_type {};                                                             \
		template <typename T>                                                                         \
		struct Name<T, decltype(void(std::declval<T*>()->Call))> : std::true_type {};

	TRACE_DEFINE_MEMBER_DETECTOR(THasDoBoost,                 DoBoost())
	TRACE_DEFINE_MEMBER_DETECTOR(THasDashCharges,             GetDashCharges())
	TRACE_DEFINE_MEMBER_DETECTOR(THasMaxDashCharges,          GetMaxDashCharges())
	TRACE_DEFINE_MEMBER_DETECTOR(THasBoostCooldownRemaining,  GetBoostCooldownRemaining())
	TRACE_DEFINE_MEMBER_DETECTOR(THasBoostCooldown,           GetBoostCooldown())
	TRACE_DEFINE_MEMBER_DETECTOR(THasPassProgress,            GetPassProgress())

	#undef TRACE_DEFINE_MEMBER_DETECTOR

	// ---------------------------------------------------------------------------------------------
	// Accessors.
	//
	// Every one of these MUST be a template, even though it is only ever instantiated with one type.
	// `if constexpr` only discards the untaken branch inside a template; in an ordinary function both
	// branches are still type-checked and the whole point is lost.
	//
	// All of them take NON-const pointers, matching the detectors, which probe `T*`. Detecting on a
	// const pointer instead would miss any accessor that was written non-const; detecting on a
	// mutable one and calling through a mutable one works whether or not the real member is const.
	// ---------------------------------------------------------------------------------------------

	/** Fires the character's boost if the mechanic exists. Returns false when it does not. */
	template <typename CharacterType>
	bool TryBoost(CharacterType* Character)
	{
		if (Character == nullptr)
		{
			return false;
		}

		if constexpr (THasDoBoost<CharacterType>::value)
		{
			Character->DoBoost();
			return true;
		}
		else
		{
			return false;
		}
	}

	template <typename CharacterType>
	constexpr bool HasBoost()
	{
		return THasDoBoost<CharacterType>::value;
	}

	/** Dash charges currently available, or @p Fallback while the charge system does not exist. */
	template <typename MovementType>
	int32 DashCharges(MovementType* Movement, int32 Fallback)
	{
		if constexpr (THasDashCharges<MovementType>::value)
		{
			return (Movement != nullptr) ? Movement->GetDashCharges() : 0;
		}
		else
		{
			return Fallback;
		}
	}

	/** Maximum dash charges, or @p Fallback. One today; two for the carrier once §5 lands. */
	template <typename MovementType>
	int32 MaxDashCharges(MovementType* Movement, int32 Fallback)
	{
		if constexpr (THasMaxDashCharges<MovementType>::value)
		{
			return (Movement != nullptr) ? Movement->GetMaxDashCharges() : Fallback;
		}
		else
		{
			return Fallback;
		}
	}

	/** Seconds until the boost is usable again. Negative means "the mechanic does not exist yet". */
	template <typename MovementType>
	float BoostCooldownRemaining(MovementType* Movement)
	{
		if constexpr (THasBoostCooldownRemaining<MovementType>::value)
		{
			return (Movement != nullptr) ? Movement->GetBoostCooldownRemaining() : -1.f;
		}
		else
		{
			return -1.f;
		}
	}

	/** The full boost cooldown, used as the meter's denominator. @p Fallback matches spec §5 (12s). */
	template <typename MovementType>
	float BoostCooldown(MovementType* Movement, float Fallback)
	{
		if constexpr (THasBoostCooldown<MovementType>::value)
		{
			return (Movement != nullptr) ? Movement->GetBoostCooldown() : Fallback;
		}
		else
		{
			return Fallback;
		}
	}

	/**
	 * Progress through the 0.5s pass hold, 0..1. Negative means "no pass in progress", which is also
	 * what a build without the mechanic reports — so the HUD ring simply never appears.
	 */
	template <typename CharacterType>
	float PassProgress(CharacterType* Character)
	{
		if constexpr (THasPassProgress<CharacterType>::value)
		{
			return (Character != nullptr) ? Character->GetPassProgress() : -1.f;
		}
		else
		{
			return -1.f;
		}
	}
}
