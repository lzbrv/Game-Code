// =================================================================================================
// Trace — TraceFixtureCore.h
//
// PUTTING THE CORE BACK WHERE A TEST FOUND IT.
//
// Several fixtures move the Core to stage their case — Trace.Ammo.CarrierTest needs a carrier and a
// non-carrier, Trace.Knife.CarrierImmunityTest pins it on a victim and forces passes, Trace.Lily's
// ZipVerify and DashTest pick it up and put it down. Each then "put it back" its own way, and two of
// the ways were wrong: the CarrierTest handed it to the pawn IT had staged as the carrier, which was
// the local player whenever the Core had been loose, and ZipVerify did not put it back at all. The
// next harness in the process then found the local player carrying, and a carrier cannot fire, swing
// a knife or reload — Trace.Knife.DualWeaponTest said NOT PROVEN, TestKnife aborted, Railgun.Verify
// failed and Trace.Ammo.BindTest waited for a usable pawn forever. Every one of them passes on its own.
//
// So there is one rule, written once:
//
//   * Capture who held it before the test touched anything.
//   * If that pawn is still alive, it gets the Core back.
//   * Otherwise (nobody held it, or the holder has since died) nobody should: a Core the fixture left
//     on somebody is put on the half-start spot, held by nobody — the same contested placement a half
//     starts with, through ATraceCore::KickoffContested. A loose Core in flight cannot be put back in
//     flight; "held by nobody" is the part the next test depends on.
//
// Dev-only, authority-only. Every fixture that calls this is itself compiled out of Shipping.
// =================================================================================================

#pragma once

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

class ATraceCharacter;
class UWorld;

namespace TraceFixtureCore
{
	/** Who held the Core when a fixture started. */
	struct FCoreHolding
	{
		/** There was a Core to capture at all. */
		bool bCaptured = false;

		/** Somebody was carrying it. */
		bool bWasHeld = false;

		TWeakObjectPtr<ATraceCharacter> Holder;

		/** For the log line; the pawn may be gone by the time it is printed. */
		FString HolderName;
	};

	/** Reads the Core's holder in @p World. Changes nothing. */
	FCoreHolding CaptureCoreHolding(const UWorld* World);

	/**
	 * Puts possession back as @p Captured describes (see the file header for the rule).
	 *
	 * @return one sentence saying what it did, for the fixture's own log line.
	 */
	FString RestoreCoreHolding(UWorld* World, const FCoreHolding& Captured);
}

#endif // !UE_BUILD_SHIPPING
