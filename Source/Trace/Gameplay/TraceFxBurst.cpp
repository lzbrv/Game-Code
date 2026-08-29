// Trace — ATraceFxBurst. See the header for the clause-by-clause reading of FX_AUDIO_PLAN §1.3 and
// for why nine effects share one actor class.

#include "Gameplay/TraceFxBurst.h"

#include "CollisionQueryParams.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"

#include "Abilities/TraceAbilityTypes.h"   // ETraceCharacterId — the id AccentOwnerFor names
#include "Audio/TraceAudio.h"
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacterRoster.h"     // THE accent, read live. See the HUES block below.
#include "Gameplay/TraceFxShapes.h"
#include "Trace.h"
#include "TraceSettings.h"

#if !UE_BUILD_SHIPPING
#include "Engine/HitResult.h"             // the crosshair trace's answer
#include "EngineUtils.h"                  // TActorIterator — Trace.Fx.LoopBudget walks the pawns
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"                 // FScreenshotRequest — Trace.Fx.BurstTest photographs itself
#endif

// =================================================================================================
// THE RED ARM.
//
// Every FX file in this project can be made to fail its own harness, because a harness that has
// never gone red is a decoration. This one arms the exact failure §1.3 names as the thing that must
// never happen: the materials do not resolve, MakeGlowMID hands back None, and the question is
// whether the burst then draws NOTHING or draws engine-default grey.
//
// It does not delete the materials — it makes MakeSolidPiece/MakeInstancedPiece behave precisely as
// they would if MakeGlowMID had returned None, which is the only branch under test. The sound must
// STILL PLAY under the arm (§1.3: "achieved blend None ⇒ invisible burst, sound still plays"), and
// Trace.Fx.BurstTest asserts both halves.
// =================================================================================================

#if !UE_BUILD_SHIPPING
/**
 * DEV ONLY. Overrides TraceFxBurstFile::EmissiveHueHeadroom so the value can be MEASURED off frames
 * instead of argued about — the same way ATraceElleGate arrived at its ring glow, by photographing a
 * ladder and reading the hue back out of the pixels. Trace.Fx.BurstTest shoots that ladder at the end
 * of its parade. Defaults to the shipping constant, so an unarmed dev build is the shipping build.
 */
static TAutoConsoleVariable<float> CVarFxBurstHueHeadroom(
	TEXT("Trace.Fx.BurstHueHeadroom"),
	0.70f,
	TEXT("Dev only. The brightest channel an emissive burst piece may be pushed to. Low keeps the hue "
	     "and dims; high blooms and washes to white. The shipping value is the default here; "
	     "Trace.Fx.BurstTest photographs a ladder of it."),
	ECVF_Cheat);

/**
 * *** THE ARM FOR THE ACCENT DESYNC, AND IT EXISTS SO THE BUG CAN BE PHOTOGRAPHED RATHER THAN
 *     DESCRIBED. ***
 *
 * Every other file in this project arms the failure it is afraid of. The failure this one shipped
 * with was not a missing material or a wrong radius: it was SEVEN COPIED COLOUR LITERALS that stopped
 * agreeing with the roster the moment the ten accents were re-spaced, so a character's ability fired
 * in one hue while his body wore another — for a whole wave, on seven of ten characters, with nothing
 * red anywhere.
 *
 * At 1 this restores those exact seven literals. Trace.Fx.BurstTest can then be run twice against the
 * same camera and the same lighting and the two sets of frames ARE the before and after; and the
 * parade's own "hue is the owner's live accent" check goes RED under it, which is what makes that
 * check worth having.
 *
 * Dev only, and never shipped at 1 — see the LegacyAccentFor table.
 */
static TAutoConsoleVariable<int32> CVarFxBurstLegacyAccents(
	TEXT("Trace.Fx.LegacyAccents"),
	0,
	TEXT("TEST ARM ONLY. 0 (shipped): an accent-owning burst reads its hue live from "
	     "TraceCharacterRoster, so it can never disagree with the body. 1: the seven pre-W6 literals "
	     "are used instead, reproducing the desync — the ability fires in last palette's hue while "
	     "the body wears this one. Trace.Fx.BurstTest photographs both and goes red under 1. "
	     "Never ship 1."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarFxBurstForceNone(
	TEXT("Trace.Fx.BurstForceNone"),
	0,
	TEXT("TEST ARM ONLY. 0 (shipped): bursts resolve their materials normally. 1: every piece of every "
	     "burst behaves as though MakeGlowMID returned None — no material, component hidden — so "
	     "Trace.Fx.BurstTest can prove that a burst with no materials draws NOTHING rather than "
	     "engine-default grey, and that its sound still plays. Never ship 1."),
	ECVF_Cheat);
#endif // !UE_BUILD_SHIPPING

// =================================================================================================
// THE NUMBERS. One place, all of them, each with the §2/§3 table row it comes from.
// =================================================================================================

namespace TraceFxBurstFile
{
	// --- HUES (ART_BIBLE §2 accents and semantic wheel, quoted in FX_AUDIO_PLAN §2's preamble) ----
	//
	// *** THE SEVEN ACCENTS ARE READ FROM THE ROSTER. THEY USED TO BE COPIED HERE, AND THE COPY WENT
	//     STALE — THAT IS THE WHOLE REASON THIS BLOCK IS SHAPED LIKE THIS. ***
	//
	// Until W6 this namespace held seven `const FLinearColor` literals transcribed out of ART_BIBLE
	// §2.3 — ChutMint #A0F9C4, MaceViolet #D3C4FF, ElleOrchid #EDADFF, RoxieEmber #FFB361,
	// MortimerSlate #A6BFED, SlimeballSlime #D4F66F, RoccoAmber #FFEF89 — with a comment saying they
	// were "canon, not tuning" and therefore safe to duplicate. They were not safe. When the ten
	// accents were re-spaced so that no character sits within 40 deg of a team hue, the roster, the
	// body materials and the ten DataAssets all moved and THESE DID NOT, so for a whole wave every
	// one of these seven characters fired its ability in last wave's hue while its body wore the new
	// one. It was photographed happening: a START ring built at (1.000, 0.860, 0.250) — the old
	// amber — on a Rocco whose body was already acid gold.
	//
	// It is the same bug class this project already has two rules about (the drawn-vs-lethal rule at
	// DefaultRadiusUUFor's RocketBurst arm: "read live from the same knob, never a copy"), so it gets
	// the same answer. AccentOf() below reads TraceCharacterRoster, which is the ONE place a
	// character's accent is written and the place the DataAssets are generated from. Re-tune an
	// accent and this file follows in the same frame; there is no second number to forget.
	//
	// The old objection to a shared colour — ATraceElleGate's "a colour added to UTraceSettings is the
	// merge conflict that loses somebody else's knob" — is untouched by this: the roster is not a
	// settings object, nothing is being ADDED to it, and this is a read.
	//
	// THE TWO THAT STAY LITERAL ARE NOT ACCENTS. Bee rounds and Poisoned are SEMANTIC wheel entries —
	// they belong to a status, not to an owner (see HueFor's own comment on why JarPop is poison green
	// and not Oyster's accent). No character owns them, so there is nothing to derive them from and
	// re-tuning a character must NOT move them.
	const FLinearColor BeeRoundsAmber(1.00f, 0.78f, 0.10f, 1.f);  // semantic: bee rounds #FFE559
	const FLinearColor PoisonedGreen(0.35f, 0.95f, 0.20f, 1.f);   // semantic: poisoned #A0F97C

	/**
	 * The character whose ACCENT a burst type wears, as a TraceCharacterRoster id.
	 *
	 * This mapping — not the colour — is what this file legitimately owns: it is the FX plan's §2
	 * table read as "whose effect is this", and it is a fact about the EFFECT rather than about the
	 * character. NoneId means "this type is semantic, ask HueFor".
	 */
	uint8 AccentOwnerFor(ETraceFxBurstType Type)
	{
		switch (Type)
		{
		case ETraceFxBurstType::ChutBash:     return static_cast<uint8>(ETraceCharacterId::Chut);
		case ETraceFxBurstType::SpikeEmbed:   return static_cast<uint8>(ETraceCharacterId::Mace);
		case ETraceFxBurstType::ElleTeleport: return static_cast<uint8>(ETraceCharacterId::Elle);
		case ETraceFxBurstType::RocketBurst:  return static_cast<uint8>(ETraceCharacterId::Roxie);
		case ETraceFxBurstType::QuakeHit:     return static_cast<uint8>(ETraceCharacterId::Mortimer);
		case ETraceFxBurstType::SlimeSplat:   return static_cast<uint8>(ETraceCharacterId::Slimeball);

		// Semantic — a STATUS, and status beats owner (bible §6.2). Deliberately not derived.
		case ETraceFxBurstType::BeeSting:
		case ETraceFxBurstType::JarPop:       return TraceCharacterRoster::NoneId;

		// GenericRing's DEFAULT owner. Rocco's second jump takes it as it is; Oyster's pull link
		// overrides it through FTraceFxBurstSpec::Tint.
		case ETraceFxBurstType::GenericRing:
		default:                              return static_cast<uint8>(ETraceCharacterId::Rocco);
		}
	}

#if !UE_BUILD_SHIPPING
	/**
	 * THE SEVEN LITERALS THIS FILE USED TO CARRY, KEPT ONLY SO THE BUG CAN BE RE-ARMED.
	 *
	 * Verbatim, including the hexes their comments claimed, so a frame shot under
	 * Trace.Fx.LegacyAccents 1 is the frame the game actually shipped before W6 and not an
	 * approximation of it. Returns false for an id that had no copy (Oyster, X, Lily — Lily's copy
	 * lived in her own kit file, not here).
	 *
	 * *** DO NOT "TIDY THIS UP" INTO THE LIVE PATH. *** It is a fixture. The live path is AccentOf.
	 */
	bool LegacyAccentFor(uint8 CharacterId, FLinearColor& Out)
	{
		switch (static_cast<ETraceCharacterId>(CharacterId))
		{
		case ETraceCharacterId::Chut:      Out = FLinearColor(0.35f, 0.95f, 0.55f, 1.f); return true;  // #A0F9C4
		case ETraceCharacterId::Mace:      Out = FLinearColor(0.65f, 0.55f, 1.00f, 1.f); return true;  // #D3C4FF
		case ETraceCharacterId::Elle:      Out = FLinearColor(0.85f, 0.42f, 1.00f, 1.f); return true;  // #EDADFF
		case ETraceCharacterId::Roxie:     Out = FLinearColor(1.00f, 0.45f, 0.12f, 1.f); return true;  // #FFB361
		case ETraceCharacterId::Mortimer:  Out = FLinearColor(0.38f, 0.52f, 0.85f, 1.f); return true;  // #A6BFED
		case ETraceCharacterId::Slimeball: Out = FLinearColor(0.66f, 0.92f, 0.16f, 1.f); return true;  // #D4F66F
		case ETraceCharacterId::Rocco:     Out = FLinearColor(1.00f, 0.86f, 0.25f, 1.f); return true;  // #FFEF89
		default:                                                                         return false;
		}
	}
#endif // !UE_BUILD_SHIPPING

	/**
	 * @p CharacterId's accent, live from the roster, with alpha forced to 1.
	 *
	 * WHITE FOR AN UNLISTED ID, and white rather than a guess on purpose: every caller here feeds a
	 * compile-time id from AccentOwnerFor, so the only way to reach the fallback is a roster that
	 * failed to resolve at all — in which case a white burst is a visible "the roster is not there",
	 * which is exactly what TraceCharacterRoster::All() has already logged one line about.
	 *
	 * The alpha is forced because FTraceFxBurstSpec::Tint uses A==0 as its "no override" sentinel and
	 * a hue that arrived with A==0 from anywhere would read as an absent override.
	 */
	FLinearColor AccentOf(uint8 CharacterId)
	{
#if !UE_BUILD_SHIPPING
		if (CVarFxBurstLegacyAccents.GetValueOnAnyThread() != 0)
		{
			FLinearColor Legacy;
			if (LegacyAccentFor(CharacterId, Legacy))
			{
				return Legacy;
			}
		}
#endif
		if (const TraceCharacterRoster::FTraceCharacterEntry* Row = TraceCharacterRoster::Find(CharacterId))
		{
			return FLinearColor(Row->Accent.R, Row->Accent.G, Row->Accent.B, 1.f);
		}
		return FLinearColor::White;
	}

	// --- THE BEAD RING ---------------------------------------------------------------------------
	//
	// Twenty-four beads: between ATraceElleGate's 20 and ATraceMortimerQuakeWave's 48, which is the
	// range this project has already established reads as a ring rather than as a dotted line.
	constexpr int32 RingBeadCount = 24;

	/**
	 * Bead RADIUS as a fraction of the RING's radius, clamped.
	 *
	 * *** MEASURED, AND IT WAS A FIXED 6 uu UNTIL THE FIRST CAPTURE RUN. *** A fixed bead is right for
	 * one ring size and wrong for the others: SpikeEmbed's ring is 26 uu, whose 24 beads have 6.8 uu
	 * of arc each — so 12 uu-wide beads overlapped into a solid annulus and the frame
	 * (frames-W3-FXBURST/crops/crop_SpikeEmbed.png, first run) photographed a filled WHITE DISC where
	 * a ring was specified. Scaling with the radius keeps a small ring reading as a ring.
	 *
	 * The floor is bible §3.4's: 4 uu radius is 8 uu across, the minimum a world-space emissive may be
	 * before TSR dissolves it. The ceiling keeps a big ring from becoming a tube.
	 */
	constexpr float RingBeadRadiusFraction = 0.13f;
	constexpr float RingBeadMaxRadiusUU = 8.f;

	/** Beads are stretched 1.2x their arc share so the seams between them close. Mortimer's number. */
	constexpr float RingBeadOverlap = 1.2f;

	// --- PER-TYPE ELEMENT COUNTS (from the §2 tables) --------------------------------------------
	constexpr int32 BashSpeedLines = 3;      // §2.2 "3 cylinders along knock dir"
	constexpr int32 SpikeSparks = 5;         // §2.4 "5 radial spark cylinders"
	constexpr int32 RocketSpokes = 8;        // §2.3 "8 radial cylinders"
	constexpr int32 SlimeBlobs = 6;          // §2.10 "6 slime spheres"
	constexpr int32 StingSparks = 4;         // §2.7 "4 spark cylinders"
	constexpr int32 JarBlobs = 6;            // §2.6 "6 tiny spheres"

	// --- PER-TYPE FIXED DIMENSIONS ---------------------------------------------------------------
	//
	// Only the PRINCIPAL radius of each type is caller-overridable (FTraceFxBurstSpec::RadiusUU);
	// these are the rest of the geometry and they are constants because §2 wrote them as constants.
	constexpr float BashWedgeBaseRadiusUU = 55.f;   // §2.2 cone base r 55 uu
	constexpr float BashWedgeHeightUU = 90.f;       // §2.2 cone h 90 uu
	constexpr float BashLineLengthUU = 120.f;       // §2.2 speed line l 120 uu
	constexpr float BashLineStaggerUU = 20.f;       // §2.2 "staggered ±20 uu"
	constexpr float BashRingStartRadiusUU = 30.f;   // §2.2 ring r 30 -> 70
	constexpr float BashRingGlow = 3.0f;            // §2.2 "emissive mint Glow 3.0 -> 0"

	constexpr float SpikeSparkLengthUU = 40.f;      // §2.4 spark l 40 uu
	constexpr float SpikeRingStartRadiusUU = 8.f;   // grows to the principal radius (26 uu)
	constexpr float SpikeRingGlow = 2.6f;           // T1 band — a small mark on a wall, not a beacon

	constexpr float TeleportColumnHeightUU = 176.f; // §2.5 "h 176 uu (≤ player silhouette ✓)"
	constexpr float TeleportColumnStartRadiusUU = 30.f;
	constexpr float TeleportColumnEndRadiusUU = 60.f;
	constexpr float TeleportRingStartRadiusUU = 40.f;
	constexpr float TeleportRingGlow = 3.5f;        // §2.5 "emissive orchid Glow 3.5 -> 0"

	constexpr float RocketSpokeLengthUU = 90.f;     // §2.3 "l 90 uu" — capped below, see UpdateBurst

	/** ...and the cap: a spoke may never reach further than this multiple of the REAL blast radius. */
	constexpr float RocketSpokeReachFactor = 1.5f;
	constexpr float RocketRingGlow = 4.2f;          // §2.3 "Glow 4.2 -> 0" — exactly the transient cap

	constexpr float QuakeWedgeHeightUU = 60.f;      // §2.8 "cone base r 35 uu, h 60 uu"

	constexpr float SlimeBlobRadiusUU = 10.f;       // §2.10 "6 slime spheres r 10 uu"
	constexpr float JarBlobRadiusUU = 8.f;          // §2.6 "6 tiny spheres (r 8 uu)"

	constexpr float StingSparkLengthUU = 30.f;      // §2.7 "4 spark cylinders (l 30 uu)"

	constexpr float GenericRingStartRadiusUU = 30.f; // §2.9 "r 30 -> 70" / §2.6 pull link

	// =============================================================================================
	// WHICH PIECES ARE ADDITIVE AND WHICH ARE EMISSIVE — the rule, and the frames that produced it
	// =============================================================================================
	//
	// THE RULE: BIG VOLUMES ARE ADDITIVE, THIN AND SMALL PIECES ARE EMISSIVE.
	//
	// It is ATraceTracer's rule, arrived at here independently and then recognised: the tracer's CORE
	// is a thin Emissive thread and its HALO SLEEVE and MUZZLE CONE — the two big ones — are Additive,
	// for exactly the reasons that apply to a burst.
	//
	//   BIG (wedge cone, teleport column, rocket shell, sting flash): ADDITIVE. These are 55-176 uu
	//   pieces that can sit between the camera and a player. Additive writes no depth, so it cannot
	//   punch a hole in the arena or hide the pawn it is decorating, and a piece faded to zero adds
	//   zero and is genuinely gone. An opaque one faded to zero is a dark matte disc.
	//
	//   THIN AND SMALL (sparks, spokes, speed lines, blobs, ring beads): EMISSIVE. Additive geometry
	//   can only ADD to what is behind it, and its intensity is clamped at 1.0 by the material — so
	//   over a bright surface a 0.5-intensity additive piece is the background plus a little, which
	//   is GREY. The first capture run measured exactly that: crop_SlimeSplat.png and
	//   crop_GenericRing.png photographed six grey spheres and a grey ring where Slimeball's and
	//   Rocco's accents were specified, and bible §6.2's "one hue per effect" was not delivered at all.
	//   M_TraceNeon's Glow scalar can push a hue past the background; nothing else here can. At
	//   8-20 uu these pieces are far too small for opacity to occlude anything that matters.
	//
	// --- ADDITIVE INTENSITIES (§2 tables; the material clamps them at 1.0 anyway) -----------------
	constexpr float BashWedgeIntensity = 0.8f;
	constexpr float TeleportColumnIntensity = 0.7f;
	constexpr float RocketShellIntensity = 0.9f;
	constexpr float QuakeWedgeIntensity = 0.7f;
	constexpr float StingFlashIntensity = 0.9f;

	// --- EMISSIVE GLOWS (bible §3.2 T1/T2 bands; every one is clamped again by the hue headroom) ---
	constexpr float BashLineGlow = 2.6f;      // T1, the trail-glow family: a motion streak
	constexpr float SpikeSparkGlow = 2.6f;    // T1: a small mark on a wall
	constexpr float RocketSpokeGlow = 3.0f;   // T1 top: the loudest thing in the set after the ring
	constexpr float StingSparkGlow = 3.0f;
	constexpr float SlimeBlobGlow = 2.2f;     // T1 low: goo, not neon
	constexpr float JarBlobGlow = 2.2f;
	constexpr float GenericRingGlow = 3.0f;   // §2.9 asked for additive I 0.5; see the rule above

	// =============================================================================================
	// THE HUE HEADROOM — why an emissive piece is not simply given the Glow §2 asks for
	// =============================================================================================
	//
	// M_TraceNeon's emissive is Colour x Glow and the arena's auto-exposure is what does the clipping.
	// Push a saturated hue far enough and EVERY channel clips, the tonemapper hands back WHITE, and
	// the effect has no colour at all — which is bible §6.2's "one hue per effect" failing in the one
	// place it is visible.
	//
	// ATraceElleGate measured this for its own rings and wrote the numbers down (TraceElleGate.cpp,
	// the RingGlow block): at Glow 3.5 a purple came back PINK, at 1.4 washed out, at 1.0 correct.
	// The first capture run of this file reproduced it exactly — crop_ChutBash.png and
	// crop_ElleTeleport.png show mint and orchid rings rendering as identical WHITE bands at the §2
	// glows of 3.0 and 3.5. Two abilities whose rings are indistinguishable is worse than either
	// being dim.
	//
	// So the Glow a piece is actually given is capped such that the BRIGHTEST CHANNEL of
	// Colour x Glow lands at this value: bright enough to clear the bloom threshold in the arena's
	// dark and to beat a lit floor, low enough that the other channels are not dragged up with it.
	// Elle's correct answer for a lone ring in the dark was a product of 0.90; this is higher because
	// a burst has to read for a fifth of a second against whatever it happens to be standing on.
	// *** 0.70, AND IT IS A MEASUREMENT. *** Trace.Fx.BurstTest's hue ladder photographed Chut's mint
	// ring at four headrooms on the arena's BRIGHTEST surface (the blue team ramp — the worst case for
	// any emissive) and the ring pixels were read back out of the frames:
	//
	//     headroom   ring RGB                 hue        SATURATION   value
	//       0.7      (201.7, 238.0, 221.0)   151.9 deg     0.1525     0.933   <- shipped
	//       1.0      (211.2, 241.7, 227.7)   152.4 deg     0.1260     0.948
	//       1.4      (218.7, 244.3, 232.7)   152.7 deg     0.1048     0.958
	//       2.0      (223.7, 246.3, 236.0)   152.8 deg     0.0921     0.966
	//
	// Monotonic, and it is the trade stated plainly: every step up buys about 1.5% brightness and
	// costs about 20% of the COLOUR. At 2.0 Chut's ring and Elle's ring are the same white band.
	// 0.70 keeps the most hue at a value of 0.93 — still the brightest thing in frame — and it sits
	// just under the 0.90 product ATraceElleGate measured its own rings to.
	// Frames: frames-W3-FXBURST/run4-bright/hue_ladder_sheet.png and its four sources.
	//
	// *** THE LADDER WAS SHOT ON THE PRE-W6 PALETTE (Chut #A0F9C4, sRGB hue 144.3) AND THE NUMBER
	//     STILL HOLDS. *** The re-space moved Chut to #A0F9A4 (hue 122.7), so the ring RGBs in the
	//     table above are that run's and not this build's. What the ladder MEASURES is not a colour,
	//     it is the rate at which saturation is spent as the brightest channel is pushed — and that
	//     rate is a property of the tonemapper, not of the hue: the cap is expressed as a product of
	//     the BRIGHTEST CHANNEL precisely so that it means the same thing for every accent, including
	//     the darkest one on the roster (Mortimer, brightest channel 0.46, whose ceiling is therefore
	//     0.70/0.46 = 1.52 rather than the 0.82 his old pale slate got). Re-shoot the ladder if the
	//     headroom is ever re-tuned; do not re-shoot it because an accent moved.
	constexpr float EmissiveHueHeadroom = 0.70f;

	/** The live value: the constant above, or the dev override when one is armed. */
	FORCEINLINE float HueHeadroom()
	{
#if !UE_BUILD_SHIPPING
		return FMath::Clamp(CVarFxBurstHueHeadroom.GetValueOnAnyThread(), 0.2f, 4.2f);
#else
		return EmissiveHueHeadroom;
#endif
	}

	// --- SUB-ELEMENT TIMINGS, as fractions of the type's animation length ------------------------
	//
	// §2 gives several elements their own shorter life inside a longer burst (the bash's speed lines
	// are 0.18 s of a 0.22 s effect). Expressed as fractions so the whole burst can be retimed by one
	// number without the elements drifting out of order.
	constexpr float BashLineLife = 0.18f / 0.22f;
	constexpr float RocketShellLife = 0.22f / 0.35f;
	constexpr float RocketSpokeLife = 0.30f / 0.35f;

	/** Grow curves: fast out of the gate, settling. Mortimer's shockwave uses the same one. */
	FORCEINLINE float EaseOut(float Alpha)
	{
		const float A = FMath::Clamp(Alpha, 0.f, 1.f);
		return 1.f - FMath::Square(1.f - A);
	}

	/** 1 until @p HoldFraction of the way through, then a linear fade to 0 at the end. */
	FORCEINLINE float FadeAfter(float Alpha, float HoldFraction)
	{
		const float A = FMath::Clamp(Alpha, 0.f, 1.f);
		const float H = FMath::Clamp(HoldFraction, 0.f, 0.99f);
		return 1.f - FMath::Clamp((A - H) / (1.f - H), 0.f, 1.f);
	}

	/**
	 * @p Count directions evenly spaced around the aim axis, lifted @p TiltDegrees out of the plane
	 * perpendicular to it. Expressed in AIM SPACE, where +Z is the burst's Direction.
	 *
	 * Tilt 0 gives a flat fan lying on a surface; tilt 90 would give @p Count copies of the axis.
	 * Everything radial in this file is one of these, which is why the pattern is deterministic and
	 * identical on every machine — a burst that scattered with a local random number generator would
	 * look different on the server and the client, and "frame-synced on every machine" is the whole
	 * point of spawning a replicated actor for it.
	 */
	TArray<FVector> RadialFan(int32 Count, float TiltDegrees)
	{
		TArray<FVector> Out;
		Out.Reserve(FMath::Max(0, Count));

		const float Tilt = FMath::DegreesToRadians(TiltDegrees);
		const float CosT = FMath::Cos(Tilt);
		const float SinT = FMath::Sin(Tilt);

		for (int32 Index = 0; Index < Count; ++Index)
		{
			// The half-step offset keeps an even count from putting two spokes on the same screen
			// axis, which is what makes an 8-spoke burst read as a star rather than as a plus sign.
			const float Angle = (2.f * PI * (static_cast<float>(Index) + 0.5f)) / static_cast<float>(FMath::Max(1, Count));
			Out.Add(FVector(FMath::Cos(Angle) * CosT, FMath::Sin(Angle) * CosT, SinT));
		}
		return Out;
	}

	/** Moves @p Ring's beads onto a circle of @p RadiusUU in its own local XY plane. */
	void PlaceRingBeads(UInstancedStaticMeshComponent* Ring, float RadiusUU)
	{
		// The bead thickness follows the ring — see RingBeadRadiusFraction for the frame that forced it.
		if (Ring == nullptr)
		{
			return;
		}

		const int32 Count = Ring->GetInstanceCount();
		if (Count <= 0)
		{
			return;
		}

		const float SafeRadius = FMath::Max(1.f, RadiusUU);
		const float BeadRadiusUU = FMath::Clamp(SafeRadius * RingBeadRadiusFraction,
			ATraceFxBurst::MinEmissiveRadiusUU, RingBeadMaxRadiusUU);
		const float BeadScaleXY = UTraceFxShapes::ShapeScaleForRadiusUU(BeadRadiusUU);
		const float ArcLengthUU = (2.f * PI * SafeRadius / static_cast<float>(Count)) * RingBeadOverlap;
		const FVector BeadScale(BeadScaleXY, BeadScaleXY, UTraceFxShapes::ShapeScaleForLengthUU(ArcLengthUU));

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float Angle = (2.f * PI * static_cast<float>(Index)) / static_cast<float>(Count);
			const float SinA = FMath::Sin(Angle);
			const float CosA = FMath::Cos(Angle);

			const FVector Offset(CosA * SafeRadius, SinA * SafeRadius, 0.f);
			const FVector Tangent(-SinA, CosA, 0.f);

			// Each bead is a cylinder lying along the ring's tangent. Same construction as the quake
			// wave's, which is the ring this project already photographed and judged.
			const FTransform BeadTransform(FRotationMatrix::MakeFromZ(Tangent).Rotator(), Offset, BeadScale);

			// The render state is marked dirty ONCE, on the last bead, not twenty-four times.
			Ring->UpdateInstanceTransform(Index, BeadTransform, /*bWorldSpace*/ false,
				/*bMarkRenderStateDirty*/ Index == Count - 1, /*bTeleport*/ true);
		}
	}

	/**
	 * Lays each instance of @p Mesh out as a cylinder starting at the origin and running @p LengthUU
	 * along its own direction from @p Dirs.
	 */
	void PlaceSpokes(UInstancedStaticMeshComponent* Mesh, const TArray<FVector>& Dirs,
		float LengthUU, float RadiusUU)
	{
		if (Mesh == nullptr || Mesh->GetInstanceCount() <= 0)
		{
			return;
		}

		const float SafeLength = FMath::Max(0.5f, LengthUU);
		const float ScaleXY = UTraceFxShapes::ShapeScaleForRadiusUU(FMath::Max(RadiusUU, ATraceFxBurst::MinEmissiveRadiusUU));
		const FVector Scale(ScaleXY, ScaleXY, UTraceFxShapes::ShapeScaleForLengthUU(SafeLength));

		const int32 Count = FMath::Min(Mesh->GetInstanceCount(), Dirs.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			// The cylinder is centred on its own origin, so a spoke that STARTS at the burst point
			// has its centre half a length out along the direction it points.
			const FTransform Spoke(
				FRotationMatrix::MakeFromZ(Dirs[Index]).Rotator(),
				Dirs[Index] * (SafeLength * 0.5f),
				Scale);

			Mesh->UpdateInstanceTransform(Index, Spoke, /*bWorldSpace*/ false,
				/*bMarkRenderStateDirty*/ Index == Count - 1, /*bTeleport*/ true);
		}
	}

	/**
	 * Throws each instance of @p Mesh @p DistanceUU out along its direction as a sphere of
	 * @p RadiusUU, squashed to @p SquashAlongDir of its size along the direction of travel.
	 */
	void PlaceBlobs(UInstancedStaticMeshComponent* Mesh, const TArray<FVector>& Dirs,
		float DistanceUU, float RadiusUU, float SquashAlongDir)
	{
		if (Mesh == nullptr || Mesh->GetInstanceCount() <= 0)
		{
			return;
		}

		const float ScaleXY = UTraceFxShapes::ShapeScaleForRadiusUU(FMath::Max(1.f, RadiusUU));
		const FVector Scale(ScaleXY, ScaleXY, ScaleXY * FMath::Clamp(SquashAlongDir, 0.1f, 1.f));

		const int32 Count = FMath::Min(Mesh->GetInstanceCount(), Dirs.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FTransform Blob(
				FRotationMatrix::MakeFromZ(Dirs[Index]).Rotator(),
				Dirs[Index] * DistanceUU,
				Scale);

			Mesh->UpdateInstanceTransform(Index, Blob, /*bWorldSpace*/ false,
				/*bMarkRenderStateDirty*/ Index == Count - 1, /*bTeleport*/ true);
		}
	}

	/**
	 * The bash's speed lines: parallel cylinders along the aim axis, offset sideways by @p Offsets,
	 * running from @p StartZ to StartZ + @p LengthUU.
	 */
	void PlaceParallelLines(UInstancedStaticMeshComponent* Mesh, const TArray<FVector>& Offsets,
		float StartZ, float LengthUU, float RadiusUU)
	{
		if (Mesh == nullptr || Mesh->GetInstanceCount() <= 0)
		{
			return;
		}

		const float SafeLength = FMath::Max(0.5f, LengthUU);
		const float ScaleXY = UTraceFxShapes::ShapeScaleForRadiusUU(FMath::Max(RadiusUU, ATraceFxBurst::MinEmissiveRadiusUU));
		const FVector Scale(ScaleXY, ScaleXY, UTraceFxShapes::ShapeScaleForLengthUU(SafeLength));

		const int32 Count = FMath::Min(Mesh->GetInstanceCount(), Offsets.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector Offset = Offsets[Index];
			const FTransform Line(
				FRotator::ZeroRotator,   // the aim node's +Z already IS the knock direction
				FVector(Offset.X, Offset.Y, StartZ + Offset.Z + SafeLength * 0.5f),
				Scale);

			Mesh->UpdateInstanceTransform(Index, Line, /*bWorldSpace*/ false,
				/*bMarkRenderStateDirty*/ Index == Count - 1, /*bTeleport*/ true);
		}
	}
}

// =================================================================================================
// Construction and replication
// =================================================================================================

ATraceFxBurst::ATraceFxBurst()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	// §1.3, exactly. bAlwaysRelevant because the victim of a bash is by definition somewhere the
	// shooter is not, and a burst culled away is a burst that did not happen for whoever needed it
	// most. SetReplicateMovement(false) because the actor never moves: the geometry animates around
	// a fixed point, and paying for movement replication on a 1.2 s transient would be waste.
	bReplicates = true;
	SetReplicateMovement(false);
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(10.f);

	// The whole thing is over in 1.2 s (bible §6.4's objective-scale ceiling), and the animation is
	// finished well before that — the tail is there so a client that received the actor late still
	// gets to see its own copy of the fade rather than a pop-out.
	InitialLifeSpan = LifeSpanSeconds;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Root->SetMobility(EComponentMobility::Movable);
}

void ATraceFxBurst::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ATraceFxBurst, Spec);
}

void ATraceFxBurst::BeginPlay()
{
	Super::BeginPlay();

	// THE AUTHORITY BUILDS HERE, and so does a client whose Spec arrived in the same bunch as the
	// actor itself. OnRep_Spec covers the case where it did not; BuildIfNeeded is idempotent and
	// does not care which of the two got there first.
	BuildIfNeeded();
}

void ATraceFxBurst::OnRep_Spec()
{
	// A client that receives the spec must build NOW, not next tick: a 0.18 s burst cannot spend a
	// frame waiting, and the sound is played from inside BuildIfNeeded so a late build is a late
	// sound too.
	BuildIfNeeded();
}

void ATraceFxBurst::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Belt and braces for the ordering the network can produce: an actor can tick before its
	// properties have landed. Idempotent, so this costs one bool test per frame once it has run.
	BuildIfNeeded();

	Elapsed += DeltaSeconds;

	const float AnimSeconds = FMath::Max(AnimSecondsFor(Spec.Type), 0.01f);
	float Alpha = FMath::Clamp(Elapsed / AnimSeconds, 0.f, 1.f);

#if !UE_BUILD_SHIPPING
	if (DebugHeldAlpha >= 0.f)
	{
		// Trace.Fx.BurstTest's photography: a headless screenshot lands one or two frames after it is
		// requested, and photographing "whatever frame that turned out to be" of a 0.2 s animation is
		// how a harness produces evidence for a claim it did not test. Held, the frame is known.
		Alpha = FMath::Clamp(DebugHeldAlpha, 0.f, 1.f);
	}
#endif

	UpdateBurst(Alpha);
}

// =================================================================================================
// THE RECIPE TABLE — every per-type fact, in switches so each one can carry its own reasoning
// =================================================================================================

const TCHAR* ATraceFxBurst::TypeName(ETraceFxBurstType Type)
{
	switch (Type)
	{
	case ETraceFxBurstType::ChutBash:     return TEXT("ChutBash");
	case ETraceFxBurstType::SpikeEmbed:   return TEXT("SpikeEmbed");
	case ETraceFxBurstType::ElleTeleport: return TEXT("ElleTeleport");
	case ETraceFxBurstType::RocketBurst:  return TEXT("RocketBurst");
	case ETraceFxBurstType::QuakeHit:     return TEXT("QuakeHit");
	case ETraceFxBurstType::SlimeSplat:   return TEXT("SlimeSplat");
	case ETraceFxBurstType::BeeSting:     return TEXT("BeeSting");
	case ETraceFxBurstType::JarPop:       return TEXT("JarPop");
	case ETraceFxBurstType::GenericRing:  return TEXT("GenericRing");
	default:                              return TEXT("<unknown>");
	}
}

bool ATraceFxBurst::ParseType(const FString& Text, ETraceFxBurstType& OutType)
{
	for (int32 Index = 0; Index < static_cast<int32>(ETraceFxBurstType::Count); ++Index)
	{
		const ETraceFxBurstType Candidate = static_cast<ETraceFxBurstType>(Index);
		if (Text.Equals(TypeName(Candidate), ESearchCase::IgnoreCase))
		{
			OutType = Candidate;
			return true;
		}
	}

	// A bare index is accepted as well, because typing "Trace.Fx.BurstTest 3" into a console with no
	// autocomplete is a thing people do and refusing it teaches nothing.
	if (Text.IsNumeric())
	{
		const int32 Index = FCString::Atoi(*Text);
		if (Index >= 0 && Index < static_cast<int32>(ETraceFxBurstType::Count))
		{
			OutType = static_cast<ETraceFxBurstType>(Index);
			return true;
		}
	}
	return false;
}

FName ATraceFxBurst::SoundEventFor(ETraceFxBurstType Type)
{
	// EVERY EVENT NAMED HERE IS DECLARED CLIENT-SIDE in Audio/TraceSoundEvents.cpp, on purpose: the
	// actor's replication is the multicast, so the play is local (PlayReplicatedLocal, §1.6.3) and a
	// stray TraceAudio::Play() on the same event can never put a second, multicast copy on top of it.
	switch (Type)
	{
	case ETraceFxBurstType::ChutBash:     return TraceSoundEvents::ChutBash;
	case ETraceFxBurstType::SpikeEmbed:   return TraceSoundEvents::MaceSpikeEmbed;
	case ETraceFxBurstType::ElleTeleport: return TraceSoundEvents::ElleTeleport;
	case ETraceFxBurstType::RocketBurst:  return TraceSoundEvents::RoxieRocketBurst;
	case ETraceFxBurstType::BeeSting:     return TraceSoundEvents::XSting;

	// THE FOUR SILENT TYPES, and none of them is an oversight — §5.1's "trigger site" column puts
	// their sound somewhere else, and doubling it here is exactly the §8.7 double-audio failure:
	//   QuakeHit     MortimerQuake is ONE World play at the blast, not one per victim knocked.
	//   SlimeSplat   SlimeballWall is a World play at the cast site (the wall, not the splash).
	//   JarPop       OysterJarBreak belongs to the poison cloud's own BeginPlay — the cloud is the
	//                replicated fact, and it covers dash-jar break, jar-jump and detonation alike.
	//   GenericRing  shared by two kits with two different sounds (RoccoJump is World at the jump
	//                accept); a burst type that carried one of them would put it on the other's ring.
	case ETraceFxBurstType::QuakeHit:
	case ETraceFxBurstType::SlimeSplat:
	case ETraceFxBurstType::JarPop:
	case ETraceFxBurstType::GenericRing:
	default:
		return NAME_None;
	}
}

FLinearColor ATraceFxBurst::HueFor(ETraceFxBurstType Type)
{
	// ONE HUE PER EFFECT (bible §6.2 invariant 2), and the priority is semantic > team > accent —
	// which is why JarPop is Poisoned green rather than Oyster's own accent and BeeSting is BeeRounds
	// amber rather than X rose: both are showing a STATUS, and status wins.
	//
	// The two semantic hues are the file's own constants; every other type wears its OWNER's accent
	// and reads it live from the roster, so a re-tune cannot leave the ability firing in last wave's
	// colour while the body wears this one. See the HUES block at the top for the wave that happened
	// in and why the literals are gone.
	switch (Type)
	{
	case ETraceFxBurstType::BeeSting:     return TraceFxBurstFile::BeeRoundsAmber;
	case ETraceFxBurstType::JarPop:       return TraceFxBurstFile::PoisonedGreen;

	// Everything else, GenericRing's DEFAULT included: the accent of the character AccentOwnerFor
	// names. Rocco's second jump takes the default as it is; Oyster's pull link passes Poisoned green
	// through FTraceFxBurstSpec::Tint. See that field's comment for why the override exists on
	// exactly one type.
	default:
		return TraceFxBurstFile::AccentOf(TraceFxBurstFile::AccentOwnerFor(Type));
	}
}

float ATraceFxBurst::AnimSecondsFor(ETraceFxBurstType Type)
{
	// §1.3: "each type is <= 0.45 s of animation + fade inside the 1.2 s lifespan". Every number here
	// is the LONGEST element in that type's §2 row, so no element is ever cut off by the timeline.
	switch (Type)
	{
	case ETraceFxBurstType::ChutBash:     return 0.22f;   // §2.2 wedge 0.22 (lines 0.18, ring 0.20)
	case ETraceFxBurstType::SpikeEmbed:   return 0.22f;   // §2.4 "0.22 s"
	case ETraceFxBurstType::ElleTeleport: return 0.30f;   // §2.5 "0.3 s" — bible cast-flash cap 0.5 ✓
	case ETraceFxBurstType::RocketBurst:  return 0.35f;   // §2.3 surface ring 0.35 (shell .22, spokes .30)
	case ETraceFxBurstType::QuakeHit:     return 0.20f;   // §2.8 "0.2 s"
	case ETraceFxBurstType::SlimeSplat:   return 0.30f;   // §2.10 "0.3 s"
	case ETraceFxBurstType::BeeSting:     return 0.18f;   // §2.7 "0.18 s"
	case ETraceFxBurstType::JarPop:       return 0.25f;   // §2.6 "fade 0.25 s"
	case ETraceFxBurstType::GenericRing:  return 0.25f;   // §2.9 second jump "0.25 s"
	default:                              return 0.25f;
	}
}

float ATraceFxBurst::DefaultRadiusUUFor(ETraceFxBurstType Type, const UObject* WorldContext)
{
	switch (Type)
	{
	case ETraceFxBurstType::ChutBash:     return 70.f;   // §2.2 contact ring "r 30 -> 70 uu"
	case ETraceFxBurstType::SpikeEmbed:   return 26.f;   // §2.4 "surface ring r 26 uu"
	case ETraceFxBurstType::ElleTeleport: return 80.f;   // §2.5 ground ring "r 40 -> 80 uu"
	case ETraceFxBurstType::QuakeHit:     return 35.f;   // §2.8 wedge "cone base r 35 uu"
	// §2.10 gives the blobs a size but no scatter distance, so this is a choice: 70 uu is far enough
	// that six 10 uu spheres separate into six spheres instead of a single lump, which is what 45
	// photographed as on the first capture run (crop_SlimeSplat.png).
	case ETraceFxBurstType::SlimeSplat:   return 70.f;
	case ETraceFxBurstType::BeeSting:     return 20.f;   // §2.7 "flash sphere r 20 uu"
	case ETraceFxBurstType::JarPop:       return 60.f;   // §2.6 "scatter 60 uu outward"
	case ETraceFxBurstType::GenericRing:  return 80.f;   // §2.9 second jump "r 30 -> 70", §2.5 40 -> 80

	case ETraceFxBurstType::RocketBurst:
	default:
		// *** DRAWN == LETHAL, READ LIVE. *** Bible §6.2 invariant 1 and FX §8.5 both say the rocket
		// burst's radius is "the damage radius knob read live from the same settings knob
		// ApplyRocketDamageTo uses — never a copy". That knob is RoxieRocketHitRadiusUU and this is
		// the identical read, with the identical clamp, that ATraceRoxieRocket does at its own hit
		// test (TraceRoxieRocket.cpp:76). Retune the ability and the burst follows in the same frame;
		// there is no second number to forget.
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketHitRadiusUU, 1.f, 300.f);
	}
}

// =================================================================================================
// THE SERVER ENTRY POINT
// =================================================================================================

ATraceFxBurst* ATraceFxBurst::Burst(UWorld* World, ETraceFxBurstType Type, const FVector& Location,
	const FVector& Direction, float RadiusUU, const FLinearColor* TintOverride)
{
	if (World == nullptr || Type >= ETraceFxBurstType::Count)
	{
		return nullptr;
	}

	// AUTHORITY ONLY, and it is worth refusing loudly rather than quietly: a client that spawns one
	// of these gets a local actor that replicates nowhere, which looks right on the machine that
	// wrote the bug and wrong on every other one — the hardest class of FX bug to find.
	if (World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[FxBurst] %s refused: ATraceFxBurst::Burst is authority-only. The burst IS the "
			     "multicast, so it has to be spawned by the machine that knows the fact."),
			TypeName(Type));
		return nullptr;
	}

	// THE ACTOR IS ALWAYS SPAWNED UNROTATED. Everything is built from the REPLICATED Spec.Direction
	// in the aim node's own frame, so nothing here depends on a spawn rotation surviving the wire —
	// and because the actor's frame is the world's, a world direction is also a local one.
	//
	// *** DEFERRED, AND THIS IS NOT A STYLE CHOICE. *** A plain SpawnActor into a world that has begun
	// play runs BeginPlay INSIDE the spawn call — so a Spec written on the line after it is written
	// after BuildIfNeeded has already run, and every burst builds as whatever the CDO's default type
	// is. That is not a subtle failure: measured on the first capture run, all nine types built the
	// GenericRing recipe at the GenericRing radius. SpawnActorDeferred holds BeginPlay until
	// FinishSpawning, which is also what puts the right type in the FIRST replicated bunch instead of
	// a GenericRing that turns into a RocketBurst a frame later on every client.
	const FTransform SpawnTransform(FRotator::ZeroRotator, Location);

	ATraceFxBurst* NewBurst = World->SpawnActorDeferred<ATraceFxBurst>(
		ATraceFxBurst::StaticClass(), SpawnTransform, /*Owner*/ nullptr, /*Instigator*/ nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (NewBurst == nullptr)
	{
		return nullptr;
	}

	NewBurst->Spec.Type = Type;
	NewBurst->Spec.RadiusUU = FMath::Max(0.f, RadiusUU);

	const FVector SafeDir = Direction.GetSafeNormal();
	NewBurst->Spec.Direction = SafeDir.IsNearlyZero() ? FVector::UpVector : SafeDir;

	if (TintOverride != nullptr)
	{
		// Alpha 255 is what marks the field as "set" — see FTraceFxBurstSpec::Tint. The quantisation
		// to 8 bits per channel is deliberate and harmless: these are constants from the bible's
		// palette, not measured values, and 4 bytes on the wire per burst instead of 16 matters more.
		FColor Packed = TintOverride->ToFColor(/*bSRGB*/ false);
		Packed.A = 255;
		NewBurst->Spec.Tint = Packed;
	}

	// FinishSpawning runs BeginPlay, which runs BuildIfNeeded — so the server sees and hears the burst
	// on the same frame it tells its clients about it, with the whole Spec already in place.
	NewBurst->FinishSpawning(SpawnTransform);

	return NewBurst;
}

// =================================================================================================
// Piece construction — the degradation ladder, applied identically to every piece of every type
// =================================================================================================

UStaticMeshComponent* ATraceFxBurst::MakeSolidPiece(USceneComponent* Parent, const TCHAR* NameHint,
	UStaticMesh* Mesh, bool bAdditiveOnly, TObjectPtr<UMaterialInstanceDynamic>& OutMID,
	ETraceFxBlend& OutBlend)
{
	OutMID = nullptr;
	OutBlend = ETraceFxBlend::None;

	if (Parent == nullptr || Mesh == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* Piece = NewObject<UStaticMeshComponent>(
		this, MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), FName(NameHint)));
	if (Piece == nullptr)
	{
		return nullptr;
	}

	Piece->SetupAttachment(Parent);
	Piece->SetStaticMesh(Mesh);

	// The shared "this is decoration" pass: no collision on any channel, no shadow, no decals, not an
	// occluder. A COLLIDING piece of FX would break hitscan for as long as it existed, which is the
	// one failure the FX library's own header says a verifier will check for.
	UTraceFxShapes::ConfigureFxComponent(Piece);
	Piece->SetCanEverAffectNavigation(false);

	Piece->RegisterComponent();

	const ETraceFxBlend Preferred = bAdditiveOnly ? ETraceFxBlend::Translucent : ETraceFxBlend::Emissive;
	UMaterialInstanceDynamic* MID = UTraceFxShapes::MakeGlowMID(Piece, 0, Preferred, OutBlend);

#if !UE_BUILD_SHIPPING
	if (CVarFxBurstForceNone.GetValueOnAnyThread() != 0)
	{
		// THE RED ARM. Behave exactly as if nothing had resolved — which is the branch under test.
		MID = nullptr;
		OutBlend = ETraceFxBlend::None;
	}
#endif

	// *** ADDITIVE OR NOTHING, for the pieces that say so. *** This is ATraceTracer's halo/muzzle
	// rule and it is here for the same measured reason: M_TraceNeon is unlit but OPAQUE, so a 45 uu
	// opaque sphere or a 55 uu opaque cone at head height writes depth and punches a hole in the
	// arena behind it — and because these effects fade on BRIGHTNESS, a faded-out opaque piece is not
	// gone, it is a dark matte disc sitting in the world for the rest of the burst. Additive writes
	// no depth: the arena stays visible through it and a piece faded to zero adds zero.
	const bool bBlendUsable = (MID != nullptr) && (OutBlend != ETraceFxBlend::None)
		&& (!bAdditiveOnly || OutBlend == ETraceFxBlend::Additive || OutBlend == ETraceFxBlend::Translucent);

	if (!bBlendUsable)
	{
		// NO GREY. An untextured 100 uu default primitive is far worse than no effect at all, and
		// "no grey primitive on a forced None" is the acceptance this tranche is measured by.
		Piece->SetVisibility(false);
		OutBlend = ETraceFxBlend::None;
		return Piece;
	}

	OutMID = MID;
	return Piece;
}

UInstancedStaticMeshComponent* ATraceFxBurst::MakeInstancedPiece(USceneComponent* Parent, const TCHAR* NameHint,
	UStaticMesh* Mesh, bool bAdditiveOnly, TObjectPtr<UMaterialInstanceDynamic>& OutMID,
	ETraceFxBlend& OutBlend)
{
	OutMID = nullptr;
	OutBlend = ETraceFxBlend::None;

	if (Parent == nullptr || Mesh == nullptr)
	{
		return nullptr;
	}

	UInstancedStaticMeshComponent* Piece = NewObject<UInstancedStaticMeshComponent>(
		this, MakeUniqueObjectName(this, UInstancedStaticMeshComponent::StaticClass(), FName(NameHint)));
	if (Piece == nullptr)
	{
		return nullptr;
	}

	Piece->SetupAttachment(Parent);

	// THE MESH MUST BE SET BEFORE ANYTHING ELSE. UInstancedStaticMeshComponent accepts every
	// AddInstance without one and reports them all back, but a static mesh component with no mesh
	// creates no scene proxy and is never handed to the renderer — which is precisely the shipped
	// "Elle's portal is invisible" bug (TraceElleGate.cpp:55-77) and the reason that file counts
	// mesh-assignment rather than instance count. Same order here, deliberately.
	Piece->SetStaticMesh(Mesh);

	UTraceFxShapes::ConfigureFxComponent(Piece);
	Piece->SetCanEverAffectNavigation(false);

	Piece->RegisterComponent();

	const ETraceFxBlend Preferred = bAdditiveOnly ? ETraceFxBlend::Translucent : ETraceFxBlend::Emissive;
	UMaterialInstanceDynamic* MID = UTraceFxShapes::MakeGlowMID(Piece, 0, Preferred, OutBlend);

#if !UE_BUILD_SHIPPING
	if (CVarFxBurstForceNone.GetValueOnAnyThread() != 0)
	{
		MID = nullptr;
		OutBlend = ETraceFxBlend::None;
	}
#endif

	const bool bBlendUsable = (MID != nullptr) && (OutBlend != ETraceFxBlend::None)
		&& (!bAdditiveOnly || OutBlend == ETraceFxBlend::Additive || OutBlend == ETraceFxBlend::Translucent);

	if (!bBlendUsable)
	{
		Piece->SetVisibility(false);
		OutBlend = ETraceFxBlend::None;
		return Piece;
	}

	OutMID = MID;
	return Piece;
}

void ATraceFxBurst::SetPieceGlow(UMaterialInstanceDynamic* MID, ETraceFxBlend Blend,
	const FLinearColor& InHue, float Intensity) const
{
	if (MID == nullptr || Blend == ETraceFxBlend::None)
	{
		return;
	}

	// THE TWO CEILINGS ARE ENFORCED HERE AND NOWHERE ELSE, so no recipe can raise either by accident.
	//
	//   1. Bible §3.2: 7.5 is the ceiling of the world and only the two goal rings sit at it; an FX
	//      transient lives at or below 4.2, the smear-head precedent.
	//   2. THE HUE HEADROOM (TraceFxBurstFile::EmissiveHueHeadroom): whichever channel of this hue is
	//      brightest may not be pushed past it, or every channel clips and the piece renders white.
	//      Measured — see the long comment on that constant, and the Elle gate's before it.
	//
	// Additive geometry is separately capped at 1.0 by the material itself (it has no Glow scalar and
	// brightness rides in the colour), so both clamps only bite on the Emissive path.
	float Ceiling = 1.f;
	if (Blend == ETraceFxBlend::Emissive || Blend == ETraceFxBlend::Fallback)
	{
		const float BrightestChannel = FMath::Max3(InHue.R, InHue.G, InHue.B);
		const float HueCeiling = (BrightestChannel > UE_KINDA_SMALL_NUMBER)
			? (HueHeadroomAtBuild / BrightestChannel)
			: MaxTransientGlow;
		Ceiling = FMath::Min(MaxTransientGlow, HueCeiling);
	}

	UTraceFxShapes::SetGlow(MID, Blend, InHue, FMath::Clamp(Intensity, 0.f, Ceiling));
}

// =================================================================================================
// BuildIfNeeded — the nine recipes
// =================================================================================================

void ATraceFxBurst::BuildIfNeeded()
{
	if (bBuilt)
	{
		return;
	}

	// A dedicated server has no shaders cooked and no audio device. It still ticks and still destroys
	// the actor on schedule, so the clients' copies are unaffected — the same early-out
	// ATraceMortimerQuakeWave takes, for the same reason.
	if (GetNetMode() == NM_DedicatedServer)
	{
		bBuilt = true;
		return;
	}

	bBuilt = true;

	Hue = HueFor(Spec.Type);
	if (Spec.Tint.A != 0 && Spec.Type == ETraceFxBurstType::GenericRing)
	{
		// ReinterpretAsLinear, NOT FLinearColor(FColor) — the latter applies an sRGB-to-linear curve,
		// and Burst() packs the tint with ToFColor(bSRGB=false), which does not. Pairing the two would
		// darken every override by the gamma curve and the wrong colour would be nobody's fault.
		Hue = Spec.Tint.ReinterpretAsLinear();
		Hue.A = 1.f;
	}

	ResolvedRadiusUU = (Spec.RadiusUU > 0.f)
		? Spec.RadiusUU
		: DefaultRadiusUUFor(Spec.Type, this);

	// LATCHED HERE, read nowhere else. See SetPieceGlow.
	HueHeadroomAtBuild = TraceFxBurstFile::HueHeadroom();

	const FVector Direction = FVector(Spec.Direction).GetSafeNormal(1.e-4f, FVector::UpVector);

	// The two aim nodes. Not primitives — they draw nothing and cost nothing — they exist so the
	// geometry can be placed with UTraceFxShapes' local-Z helpers, which only mean what they say when
	// the PARENT's +Z is the axis. See the header's comment on Aim.
	Aim = NewObject<USceneComponent>(this, TEXT("BurstAim"));
	if (Aim != nullptr)
	{
		Aim->SetupAttachment(Root);
		Aim->SetRelativeRotation(FRotationMatrix::MakeFromZ(Direction).Rotator());
		Aim->RegisterComponent();
	}

	UStaticMesh* const Cylinder = UTraceFxShapes::GetCylinder();
	UStaticMesh* const Cone = UTraceFxShapes::GetCone();
	UStaticMesh* const Sphere = UTraceFxShapes::GetSphere();

	// A cone's apex is up its own local +Z, and every wedge in §2 has its APEX at the burst point
	// with its base out along the knock direction — so the cone's axis runs the other way from
	// everything else's and it gets a node of its own rather than a hand-rolled transform that would
	// bypass PlaceConeAlongLocalZ's measured pivot correction.
	const bool bNeedsBackAim = (Spec.Type == ETraceFxBurstType::ChutBash)
		|| (Spec.Type == ETraceFxBurstType::QuakeHit);
	if (bNeedsBackAim)
	{
		AimBack = NewObject<USceneComponent>(this, TEXT("BurstAimBack"));
		if (AimBack != nullptr)
		{
			AimBack->SetupAttachment(Root);
			AimBack->SetRelativeRotation(FRotationMatrix::MakeFromZ(-Direction).Rotator());
			AimBack->RegisterComponent();
		}
	}

	switch (Spec.Type)
	{
	case ETraceFxBurstType::ChutBash:
	{
		// §2.2: shock wedge + three speed lines + a contact ring in Chut's accent.
		ShapeA = MakeSolidPiece(AimBack, TEXT("BashWedge"), Cone, /*bAdditiveOnly*/ true, ShapeAMID, ShapeABlend);

		ScatterMesh = MakeInstancedPiece(Aim, TEXT("BashLines"), Cylinder, /*bAdditiveOnly*/ false, ScatterMID, ScatterBlend);
		if (ScatterMesh != nullptr)
		{
			// "staggered ±20 uu" — a fixed, deterministic stagger rather than a random one, so the
			// server and every client draw the same three lines (see RadialFan's note).
			ScatterOffsets.Add(FVector(TraceFxBurstFile::BashLineStaggerUU, -0.5f * TraceFxBurstFile::BashLineStaggerUU, 0.f));
			ScatterOffsets.Add(FVector(-0.8f * TraceFxBurstFile::BashLineStaggerUU, 0.6f * TraceFxBurstFile::BashLineStaggerUU, 18.f));
			ScatterOffsets.Add(FVector(0.1f * TraceFxBurstFile::BashLineStaggerUU, TraceFxBurstFile::BashLineStaggerUU, -16.f));
			for (int32 Index = 0; Index < TraceFxBurstFile::BashSpeedLines; ++Index)
			{
				ScatterMesh->AddInstance(FTransform::Identity);
			}
		}

		RingMesh = MakeInstancedPiece(Aim, TEXT("BashRing"), Cylinder, /*bAdditiveOnly*/ false, RingMID, RingBlend);
		break;
	}

	case ETraceFxBurstType::SpikeEmbed:
	{
		// §2.4: five sparks in Mace's accent off the surface, plus a small surface ring at the anchor.
		ScatterMesh = MakeInstancedPiece(Aim, TEXT("SpikeSparks"), Cylinder, /*bAdditiveOnly*/ false, ScatterMID, ScatterBlend);
		if (ScatterMesh != nullptr)
		{
			// 30 degrees out of the wall plane: flat enough to read as "off the surface", steep
			// enough that the sparks are not buried in the wall they came out of.
			ScatterDirs = TraceFxBurstFile::RadialFan(TraceFxBurstFile::SpikeSparks, 30.f);
			for (int32 Index = 0; Index < ScatterDirs.Num(); ++Index)
			{
				ScatterMesh->AddInstance(FTransform::Identity);
			}
		}

		RingMesh = MakeInstancedPiece(Aim, TEXT("SpikeRing"), Cylinder, /*bAdditiveOnly*/ false, RingMID, RingBlend);
		break;
	}

	case ETraceFxBurstType::ElleTeleport:
	{
		// §2.5: a column the height of a player, and a ground ring at the feet.
		//
		// SPAWN THIS ONE AT THE CAPSULE CENTRE. The gate stores its mouth there (ATraceElleGate's
		// ArrivalLift comment says so), the column is centred on the burst point, and the ring is
		// dropped half a player below it — so a burst spawned at a mouth wraps the arriving pawn and
		// puts its ring on the floor. Both pieces hang off Root, not Aim: a teleport column stands up
		// in the world regardless of which way anybody was facing.
		ShapeA = MakeSolidPiece(Root, TEXT("TeleportColumn"), Cylinder, /*bAdditiveOnly*/ true, ShapeAMID, ShapeABlend);

		RingMesh = MakeInstancedPiece(Root, TEXT("TeleportRing"), Cylinder, /*bAdditiveOnly*/ false, RingMID, RingBlend);
		if (RingMesh != nullptr)
		{
			RingMesh->SetRelativeLocation(FVector(0.f, 0.f, -0.5f * TraceFxBurstFile::TeleportColumnHeightUU));
		}
		break;
	}

	case ETraceFxBurstType::RocketBurst:
	{
		// §2.3: a shell at the REAL blast radius, eight spokes, a surface ring on the hit normal.
		ShapeA = MakeSolidPiece(Root, TEXT("RocketShell"), Sphere, /*bAdditiveOnly*/ true, ShapeAMID, ShapeABlend);

		ScatterMesh = MakeInstancedPiece(Aim, TEXT("RocketSpokes"), Cylinder, /*bAdditiveOnly*/ false, ScatterMID, ScatterBlend);
		if (ScatterMesh != nullptr)
		{
			// 35 degrees off the surface: a splash leaving a wall, not a flat star painted on it.
			ScatterDirs = TraceFxBurstFile::RadialFan(TraceFxBurstFile::RocketSpokes, 35.f);
			for (int32 Index = 0; Index < ScatterDirs.Num(); ++Index)
			{
				ScatterMesh->AddInstance(FTransform::Identity);
			}
		}

		RingMesh = MakeInstancedPiece(Aim, TEXT("RocketRing"), Cylinder, /*bAdditiveOnly*/ false, RingMID, RingBlend);
		break;
	}

	case ETraceFxBurstType::QuakeHit:
	{
		// §2.8: one small wedge per knocked victim, in Mortimer's accent (patinated steel since the
		// W5 re-space; it was slate, and nothing here names the colour any more so it cannot go stale
		// again). Deliberately the cheapest of the nine —
		// a quake can knock eight people at once and eight bursts have to stay affordable.
		ShapeA = MakeSolidPiece(AimBack, TEXT("QuakeWedge"), Cone, /*bAdditiveOnly*/ true, ShapeAMID, ShapeABlend);
		break;
	}

	case ETraceFxBurstType::SlimeSplat:
	{
		// §2.10: six blobs scattering off the base of a rising wall.
		ScatterMesh = MakeInstancedPiece(Aim, TEXT("SlimeBlobs"), Sphere, /*bAdditiveOnly*/ false, ScatterMID, ScatterBlend);
		if (ScatterMesh != nullptr)
		{
			ScatterDirs = TraceFxBurstFile::RadialFan(TraceFxBurstFile::SlimeBlobs, 40.f);
			for (int32 Index = 0; Index < ScatterDirs.Num(); ++Index)
			{
				ScatterMesh->AddInstance(FTransform::Identity);
			}
		}
		break;
	}

	case ETraceFxBurstType::BeeSting:
	{
		// §2.7: a flash and four sparks, over in 0.18 s — the shortest burst in the set, because a
		// sting is a pinprick and a long one would read as an explosion.
		ShapeA = MakeSolidPiece(Root, TEXT("StingFlash"), Sphere, /*bAdditiveOnly*/ true, ShapeAMID, ShapeABlend);

		ScatterMesh = MakeInstancedPiece(Aim, TEXT("StingSparks"), Cylinder, /*bAdditiveOnly*/ false, ScatterMID, ScatterBlend);
		if (ScatterMesh != nullptr)
		{
			ScatterDirs = TraceFxBurstFile::RadialFan(TraceFxBurstFile::StingSparks, 25.f);
			for (int32 Index = 0; Index < ScatterDirs.Num(); ++Index)
			{
				ScatterMesh->AddInstance(FTransform::Identity);
			}
		}
		break;
	}

	case ETraceFxBurstType::JarPop:
	{
		// §2.6: six tiny spheres thrown 60 uu outward as a jar breaks. Poisoned green — the CLOUD is
		// the lethal volume and it draws itself; this is only the pop that starts it.
		ScatterMesh = MakeInstancedPiece(Aim, TEXT("JarBlobs"), Sphere, /*bAdditiveOnly*/ false, ScatterMID, ScatterBlend);
		if (ScatterMesh != nullptr)
		{
			// 55 degrees: a jar bursts upward and outward, not sideways along the floor.
			ScatterDirs = TraceFxBurstFile::RadialFan(TraceFxBurstFile::JarBlobs, 55.f);
			for (int32 Index = 0; Index < ScatterDirs.Num(); ++Index)
			{
				ScatterMesh->AddInstance(FTransform::Identity);
			}
		}
		break;
	}

	case ETraceFxBurstType::GenericRing:
	default:
	{
		// §2.9 (Rocco's second jump) and §2.6 (Oyster's pull link). ADDITIVE, not emissive: §2.9 asks
		// for "amber additive I 0.5", and an additive ring under a jumping pawn cannot occlude the
		// pawn's own feet the way an opaque one would.
		RingMesh = MakeInstancedPiece(Aim, TEXT("GenericRing"), Cylinder, /*bAdditiveOnly*/ false, RingMID, RingBlend);
		break;
	}
	}

	// Every ring gets its beads here rather than in nine places.
	if (RingMesh != nullptr)
	{
		for (int32 Index = 0; Index < TraceFxBurstFile::RingBeadCount; ++Index)
		{
			RingMesh->AddInstance(FTransform::Identity);
		}
	}

	// --- THE SOUND -------------------------------------------------------------------------------
	//
	// LOCAL, WITH NO RPC (§1.6.3). This code is running on every machine already, because the actor
	// replicated to every machine — adding a multicast here would play the sound twice on every
	// client and once more on the host. It plays whether or not anything above resolved a material:
	// §1.3 is explicit that an invisible burst is still an audible one.
	const FName Event = SoundEventFor(Spec.Type);
	if (!Event.IsNone())
	{
		// MEASURED, NOT ASSUMED. "I called the play function" and "a sound reached the engine" are
		// different claims, and the second is the one anybody cares about — the subsystem's per-event
		// map is bumped inside PlayLocalNow/PlayWorldNow, i.e. AFTER the side gate, the settings gate,
		// the device test and the resolve. Reading it either side of the call is the difference
		// between a harness that checks itself and one that checks the game.
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(this);
		const int32 PlaysBefore = (Audio != nullptr) ? Audio->GetPlaysByEvent().FindRef(Event) : 0;

		TraceAudio::PlayReplicatedLocal(this, Event, GetActorLocation());

		const int32 PlaysAfter = (Audio != nullptr) ? Audio->GetPlaysByEvent().FindRef(Event) : 0;
		bSoundPlayed = (PlaysAfter > PlaysBefore);
	}

	// Frame zero of the animation, so the first frame anybody sees is the start of the burst and not
	// whatever size the pieces happened to be created at.
	UpdateBurst(0.f);

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[FxBurst] %s built at %s: radius %.0f uu, %d primitive(s) (%d visible), %s, sound %s."),
		TypeName(Spec.Type), *GetActorLocation().ToCompactString(), ResolvedRadiusUU,
		GetPrimitiveCount(), GetVisiblePrimitiveCount(), *DescribeBlends(),
		Event.IsNone() ? TEXT("<none by design>") : *Event.ToString());
}

// =================================================================================================
// UpdateBurst — the timeline. Every curve here is MONOTONIC: sizes grow, brightness falls.
// Nothing oscillates, because a burst has no state to communicate and bible §3.3 forbids a
// brightness pulse on anything that reads as a lethal fact.
// =================================================================================================

void ATraceFxBurst::UpdateBurst(float Alpha)
{
	using namespace TraceFxBurstFile;

	const float A = FMath::Clamp(Alpha, 0.f, 1.f);

	switch (Spec.Type)
	{
	case ETraceFxBurstType::ChutBash:
	{
		if (ShapeA != nullptr && ShapeABlend != ETraceFxBlend::None)
		{
			// The wedge snaps to full size in the first third and then fades: a shove is instant, and
			// a wedge that grew over the whole burst would read as an expanding volume instead.
			const float Grow = 0.55f + 0.45f * EaseOut(A / 0.35f);
			UTraceFxShapes::PlaceConeAlongLocalZ(ShapeA,
				-BashWedgeHeightUU * Grow, BashWedgeBaseRadiusUU * Grow, BashWedgeHeightUU * Grow);
			SetPieceGlow(ShapeAMID, ShapeABlend, Hue, BashWedgeIntensity * (1.f - A));
		}

		if (ScatterMesh != nullptr && ScatterBlend != ETraceFxBlend::None)
		{
			// The lines travel forward as they fade — motion, which is what says "he went that way".
			const float LineAlpha = FMath::Clamp(A / BashLineLife, 0.f, 1.f);
			PlaceParallelLines(ScatterMesh, ScatterOffsets,
				/*StartZ*/ 40.f * LineAlpha, BashLineLengthUU, MinEmissiveRadiusUU);
			SetPieceGlow(ScatterMID, ScatterBlend, Hue, BashLineGlow * (1.f - LineAlpha));
		}

		if (RingMesh != nullptr && RingBlend != ETraceFxBlend::None)
		{
			const float RingAlpha = FMath::Clamp(A / (0.20f / 0.22f), 0.f, 1.f);
			PlaceRingBeads(RingMesh, FMath::Lerp(BashRingStartRadiusUU, ResolvedRadiusUU, EaseOut(RingAlpha)));
			SetPieceGlow(RingMID, RingBlend, Hue, BashRingGlow * FadeAfter(RingAlpha, 0.45f));
		}
		break;
	}

	case ETraceFxBurstType::SpikeEmbed:
	{
		if (ScatterMesh != nullptr && ScatterBlend != ETraceFxBlend::None)
		{
			PlaceSpokes(ScatterMesh, ScatterDirs,
				SpikeSparkLengthUU * EaseOut(A / 0.35f), MinEmissiveRadiusUU);
			SetPieceGlow(ScatterMID, ScatterBlend, Hue, SpikeSparkGlow * (1.f - A));
		}

		if (RingMesh != nullptr && RingBlend != ETraceFxBlend::None)
		{
			PlaceRingBeads(RingMesh, FMath::Lerp(SpikeRingStartRadiusUU, ResolvedRadiusUU, EaseOut(A / 0.6f)));
			SetPieceGlow(RingMID, RingBlend, Hue, SpikeRingGlow * FadeAfter(A, 0.45f));
		}
		break;
	}

	case ETraceFxBurstType::ElleTeleport:
	{
		if (ShapeA != nullptr && ShapeABlend != ETraceFxBlend::None)
		{
			const float ColumnRadius = FMath::Lerp(TeleportColumnStartRadiusUU, TeleportColumnEndRadiusUU, EaseOut(A));
			UTraceFxShapes::StretchAlongLocalZ(ShapeA,
				-0.5f * TeleportColumnHeightUU, 0.5f * TeleportColumnHeightUU, ColumnRadius);
			SetPieceGlow(ShapeAMID, ShapeABlend, Hue, TeleportColumnIntensity * (1.f - A));
		}

		if (RingMesh != nullptr && RingBlend != ETraceFxBlend::None)
		{
			PlaceRingBeads(RingMesh, FMath::Lerp(TeleportRingStartRadiusUU, ResolvedRadiusUU, EaseOut(A)));
			SetPieceGlow(RingMID, RingBlend, Hue, TeleportRingGlow * FadeAfter(A, 0.40f));
		}
		break;
	}

	case ETraceFxBurstType::RocketBurst:
	{
		if (ShapeA != nullptr && ShapeABlend != ETraceFxBlend::None)
		{
			// (a) THE SHELL IS THE LETHAL VOLUME. It expands to ResolvedRadiusUU, which came from the
			// live damage knob — so what a player sees is what killed them.
			const float ShellAlpha = FMath::Clamp(A / RocketShellLife, 0.f, 1.f);
			const float ShellScale = UTraceFxShapes::ShapeScaleForRadiusUU(ResolvedRadiusUU * EaseOut(ShellAlpha));
			ShapeA->SetRelativeScale3D(FVector(ShellScale, ShellScale, ShellScale));
			SetPieceGlow(ShapeAMID, ShapeABlend, Hue, RocketShellIntensity * (1.f - ShellAlpha));
		}

		if (ScatterMesh != nullptr && ScatterBlend != ETraceFxBlend::None)
		{
			// (b) THE SPOKES ARE DECORATION AND ARE CAPPED SO THEY STAY THAT WAY.
			//
			// §2.3 asks for 90 uu, which was exactly TWICE the 45 uu blast shipping when this cap was
			// written. Photographed at that length (frames-W3-FXBURST/run2-mid, crop_RocketBurst.png)
			// the eight spokes are the loudest thing in the burst and the blast reads as twice the
			// size that can actually kill you — which is bible §6.2 invariant 1 failing in the
			// direction that matters, because a player learns the wrong distance to stand at. Capped
			// at 1.5x the REAL radius: still a spray that leaves the shell, no longer a claim about
			// reach. The two radius-true elements, (a) and (c), remain the volume read.
			//
			// DEMO 29 §7 RAISED RoxieRocketHitRadiusUU 45 -> 72 AND MADE IT A REAL BLAST RADIUS, so
			// the cap no longer binds on the rocket: 1.5 x 72 = 108, the §2.3 literal of 90 is the
			// smaller of the two, and the spokes now reach 1.25x the shell instead of 2x. Nothing here
			// changed — the cap is written against the LIVE radius on purpose, so it followed.
			const float SpokeAlpha = FMath::Clamp(A / RocketSpokeLife, 0.f, 1.f);
			const float SpokeLength = FMath::Min(RocketSpokeLengthUU, RocketSpokeReachFactor * ResolvedRadiusUU);
			PlaceSpokes(ScatterMesh, ScatterDirs,
				SpokeLength * EaseOut(SpokeAlpha / 0.4f), MinEmissiveRadiusUU);
			SetPieceGlow(ScatterMID, ScatterBlend, Hue, RocketSpokeGlow * (1.f - SpokeAlpha));
		}

		if (RingMesh != nullptr && RingBlend != ETraceFxBlend::None)
		{
			// (c) the surface ring, radius-true like the shell, and the slowest of the three so the
			// blast leaves a mark on the wall for a moment after the fire is out.
			PlaceRingBeads(RingMesh, ResolvedRadiusUU * EaseOut(A));
			SetPieceGlow(RingMID, RingBlend, Hue, RocketRingGlow * FadeAfter(A, 0.45f));
		}
		break;
	}

	case ETraceFxBurstType::QuakeHit:
	{
		if (ShapeA != nullptr && ShapeABlend != ETraceFxBlend::None)
		{
			const float Grow = 0.5f + 0.5f * EaseOut(A / 0.4f);
			UTraceFxShapes::PlaceConeAlongLocalZ(ShapeA,
				-QuakeWedgeHeightUU * Grow, ResolvedRadiusUU * Grow, QuakeWedgeHeightUU * Grow);
			SetPieceGlow(ShapeAMID, ShapeABlend, Hue, QuakeWedgeIntensity * (1.f - A));
		}
		break;
	}

	case ETraceFxBurstType::SlimeSplat:
	{
		if (ScatterMesh != nullptr && ScatterBlend != ETraceFxBlend::None)
		{
			// "scatter + squash": the blobs flatten along the way they are travelling, which is what
			// makes six spheres read as thrown goo instead of as six spheres.
			PlaceBlobs(ScatterMesh, ScatterDirs,
				ResolvedRadiusUU * EaseOut(A), SlimeBlobRadiusUU, FMath::Lerp(1.f, 0.55f, A));
			SetPieceGlow(ScatterMID, ScatterBlend, Hue, SlimeBlobGlow * FadeAfter(A, 0.5f));
		}
		break;
	}

	case ETraceFxBurstType::BeeSting:
	{
		if (ShapeA != nullptr && ShapeABlend != ETraceFxBlend::None)
		{
			const float FlashRadius = ResolvedRadiusUU * (0.3f + 0.7f * EaseOut(A / 0.3f));
			const float FlashScale = UTraceFxShapes::ShapeScaleForRadiusUU(FlashRadius);
			ShapeA->SetRelativeScale3D(FVector(FlashScale, FlashScale, FlashScale));
			SetPieceGlow(ShapeAMID, ShapeABlend, Hue, StingFlashIntensity * (1.f - A));
		}

		if (ScatterMesh != nullptr && ScatterBlend != ETraceFxBlend::None)
		{
			PlaceSpokes(ScatterMesh, ScatterDirs,
				StingSparkLengthUU * EaseOut(A / 0.3f), MinEmissiveRadiusUU);
			SetPieceGlow(ScatterMID, ScatterBlend, Hue, StingSparkGlow * (1.f - A));
		}
		break;
	}

	case ETraceFxBurstType::JarPop:
	{
		if (ScatterMesh != nullptr && ScatterBlend != ETraceFxBlend::None)
		{
			PlaceBlobs(ScatterMesh, ScatterDirs,
				ResolvedRadiusUU * EaseOut(A), JarBlobRadiusUU, /*SquashAlongDir*/ 1.f);
			SetPieceGlow(ScatterMID, ScatterBlend, Hue, JarBlobGlow * FadeAfter(A, 0.4f));
		}
		break;
	}

	case ETraceFxBurstType::GenericRing:
	default:
	{
		if (RingMesh != nullptr && RingBlend != ETraceFxBlend::None)
		{
			PlaceRingBeads(RingMesh, FMath::Lerp(GenericRingStartRadiusUU, ResolvedRadiusUU, EaseOut(A)));
			SetPieceGlow(RingMID, RingBlend, Hue, GenericRingGlow * FadeAfter(A, 0.45f));
		}
		break;
	}
	}
}

// =================================================================================================
// Queries
// =================================================================================================

int32 ATraceFxBurst::GetPrimitiveCount() const
{
	int32 Count = 0;
	Count += (ShapeA != nullptr) ? 1 : 0;
	Count += (ScatterMesh != nullptr) ? 1 : 0;
	Count += (RingMesh != nullptr) ? 1 : 0;
	return Count;
}

int32 ATraceFxBurst::GetVisiblePrimitiveCount() const
{
	// "Visible" means BOTH halves of the contract: a resolved blend AND a component that is actually
	// visible. Either alone would be a claim rather than a measurement — a piece can have a material
	// and be hidden, and (the failure this counts) a piece can be visible with no material at all,
	// which is engine grey.
	int32 Count = 0;
	if (ShapeA != nullptr && ShapeABlend != ETraceFxBlend::None && ShapeA->IsVisible())
	{
		++Count;
	}
	if (ScatterMesh != nullptr && ScatterBlend != ETraceFxBlend::None && ScatterMesh->IsVisible())
	{
		++Count;
	}
	if (RingMesh != nullptr && RingBlend != ETraceFxBlend::None && RingMesh->IsVisible())
	{
		++Count;
	}
	return Count;
}

FString ATraceFxBurst::DescribeBlends() const
{
	TArray<FString> Parts;
	if (ShapeA != nullptr)
	{
		Parts.Add(FString::Printf(TEXT("solid=%s%s"), UTraceFxShapes::BlendName(ShapeABlend),
			ShapeA->IsVisible() ? TEXT("") : TEXT("(hidden)")));
	}
	if (ScatterMesh != nullptr)
	{
		Parts.Add(FString::Printf(TEXT("scatter=%s%s x%d"), UTraceFxShapes::BlendName(ScatterBlend),
			ScatterMesh->IsVisible() ? TEXT("") : TEXT("(hidden)"), ScatterMesh->GetInstanceCount()));
	}
	if (RingMesh != nullptr)
	{
		Parts.Add(FString::Printf(TEXT("ring=%s%s x%d"), UTraceFxShapes::BlendName(RingBlend),
			RingMesh->IsVisible() ? TEXT("") : TEXT("(hidden)"), RingMesh->GetInstanceCount()));
	}
	return Parts.Num() > 0 ? FString::Join(Parts, TEXT(" ")) : TEXT("<no pieces>");
}

#if !UE_BUILD_SHIPPING
void ATraceFxBurst::DebugHoldAt(float Alpha, float ExtraLifeSeconds)
{
	DebugHeldAlpha = FMath::Clamp(Alpha, 0.f, 1.f);
	if (ExtraLifeSeconds > 0.f && HasAuthority())
	{
		SetLifeSpan(ExtraLifeSeconds);
	}
	UpdateBurst(DebugHeldAlpha);
}
#endif

// =================================================================================================
// FX_AUDIO_PLAN §1.4 — THE ATTACHED-LOOP FX BUDGET
//
// See the header for what this is and why it lives in this file. The registry is a weak map, so a
// pawn that dies with pieces still attached leaks nothing — but it does show up in
// Trace.Fx.LoopBudget as a kit that forgot its detach, which is the point.
// =================================================================================================

namespace TraceFxLoopBudgetFile
{
	// THE STORAGE LIVES IN A FILE-NAMED NAMESPACE, NOT AN ANONYMOUS ONE. TraceFxLoopBudget is
	// declared in a header and any other .cpp may open it; a `namespace TraceFxLoopBudget {
	// namespace { ... } }` here would put these at TraceFxLoopBudget::<anonymous>::G*, and the day a
	// second file does the same the unity build concatenates both and the compiler rejects the
	// redefinition. That is this project's C2084 lesson (Scripts/build.sh guards for it) and the fix
	// it names is to name the namespace after its file.
	struct FPawnLoopEntry
	{
		TWeakObjectPtr<const APawn> Pawn;
		TArray<TWeakObjectPtr<UPrimitiveComponent>> Pieces;
	};

	TArray<FPawnLoopEntry> GRegistry;

	bool bClampLogged = false;

	/** Drops dead pawns and dead components. Called by every entry point, so the map cannot rot. */
	void Prune()
	{
		for (int32 Index = GRegistry.Num() - 1; Index >= 0; --Index)
		{
			FPawnLoopEntry& Entry = GRegistry[Index];
			if (!Entry.Pawn.IsValid())
			{
				GRegistry.RemoveAtSwap(Index);
				continue;
			}
			Entry.Pieces.RemoveAll([](const TWeakObjectPtr<UPrimitiveComponent>& Piece)
			{
				return !Piece.IsValid();
			});
			if (Entry.Pieces.Num() == 0)
			{
				GRegistry.RemoveAtSwap(Index);
			}
		}
	}

	FPawnLoopEntry* Find(const APawn* Pawn)
	{
		for (FPawnLoopEntry& Entry : GRegistry)
		{
			if (Entry.Pawn.Get() == Pawn)
			{
				return &Entry;
			}
		}
		return nullptr;
	}
}

namespace TraceFxLoopBudget
{
	float ClampIntensity(float Requested)
	{
		const float Clamped = FMath::Clamp(Requested, 0.f, MaxIntensity);
		if (!FMath::IsNearlyEqual(Clamped, Requested) && !TraceFxLoopBudgetFile::bClampLogged)
		{
			TraceFxLoopBudgetFile::bClampLogged = true;
			UE_LOG(LogTraceGame, Display,
				TEXT("[FxLoop] intensity %.2f clamped to %.2f — FX_AUDIO_PLAN §1.4 caps a while-active "
				     "effect at %.2f. One line per process; the clamp keeps happening."),
				Requested, Clamped, MaxIntensity);
		}
		return Clamped;
	}

	FVector ClampToFootprint(const FVector& LocalOffset)
	{
		// RADIAL clamp only. §1.4 says "inside the capsule footprint (<= 96 uu from the capsule
		// AXIS)", which is a statement about horizontal distance: Lily's aura rings are supposed to
		// travel from chest height down to the feet, and clamping Z would delete that motion.
		const FVector2D Horizontal(LocalOffset.X, LocalOffset.Y);
		const float Distance = Horizontal.Size();
		if (Distance <= MaxOffsetFromAxisUU || Distance <= UE_KINDA_SMALL_NUMBER)
		{
			return LocalOffset;
		}

		const FVector2D Pulled = Horizontal * (MaxOffsetFromAxisUU / Distance);
		return FVector(Pulled.X, Pulled.Y, LocalOffset.Z);
	}

	int32 CountFor(const APawn* Pawn)
	{
		TraceFxLoopBudgetFile::Prune();
		const TraceFxLoopBudgetFile::FPawnLoopEntry* Entry = TraceFxLoopBudgetFile::Find(Pawn);
		return (Entry != nullptr) ? Entry->Pieces.Num() : 0;
	}

	bool CanAttach(const APawn* Pawn, int32 Additional)
	{
		if (Pawn == nullptr || Additional <= 0)
		{
			return false;
		}
		return (CountFor(Pawn) + Additional) <= MaxPrimitivesPerPawn;
	}

	UStaticMeshComponent* AttachLoopPrimitive(APawn* Pawn, USceneComponent* AttachTo,
		UStaticMesh* Mesh, const TCHAR* NameHint, const FLinearColor& Hue, float Intensity,
		const FVector& LocalOffset, float RadiusUU, TObjectPtr<UMaterialInstanceDynamic>& OutMID)
	{
		OutMID = nullptr;

		if (Pawn == nullptr || AttachTo == nullptr || Mesh == nullptr)
		{
			return nullptr;
		}

		// The attach point has to belong to the pawn being charged for it, or the budget is a number
		// about nothing. This is the one structural mistake the helper can catch for a caller.
		if (AttachTo->GetOwner() != Pawn)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[FxLoop] refused '%s': the attach point belongs to %s, not to the pawn %s being "
				     "charged for it."), NameHint, *GetNameSafe(AttachTo->GetOwner()), *GetNameSafe(Pawn));
			return nullptr;
		}

		if (!CanAttach(Pawn, 1))
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[FxLoop] refused '%s' on %s: already carrying %d attached loop primitive(s) and "
				     "§1.4 allows %d. Detach something on its off-edge."),
				NameHint, *GetNameSafe(Pawn), CountFor(Pawn), MaxPrimitivesPerPawn);
			return nullptr;
		}

		UStaticMeshComponent* Piece = NewObject<UStaticMeshComponent>(
			Pawn, MakeUniqueObjectName(Pawn, UStaticMeshComponent::StaticClass(), FName(NameHint)));
		if (Piece == nullptr)
		{
			return nullptr;
		}

		Piece->SetupAttachment(AttachTo);
		Piece->SetStaticMesh(Mesh);
		UTraceFxShapes::ConfigureFxComponent(Piece);
		Piece->SetCanEverAffectNavigation(false);

		const float SafeRadius = FMath::Max(RadiusUU, ATraceFxBurst::MinEmissiveRadiusUU);
		const float Scale = UTraceFxShapes::ShapeScaleForRadiusUU(SafeRadius);
		Piece->SetRelativeLocation(ClampToFootprint(LocalOffset));
		Piece->SetRelativeScale3D(FVector(Scale, Scale, Scale));

		Piece->RegisterComponent();

		// ADDITIVE ONLY — §1.4 says so in as many words, and the reason is the same one the tracer's
		// halo gives: a while-active effect sits ON a pawn for seconds at a time, and an opaque one
		// would hide the pawn it is decorating. Translucent resolves to Additive in this project; the
		// opaque rungs below it are refused rather than accepted.
		ETraceFxBlend Blend = ETraceFxBlend::None;
		UMaterialInstanceDynamic* MID = UTraceFxShapes::MakeGlowMID(Piece, 0, ETraceFxBlend::Translucent, Blend);
		const bool bUsable = (MID != nullptr)
			&& (Blend == ETraceFxBlend::Additive || Blend == ETraceFxBlend::Translucent);
		if (!bUsable)
		{
			// No grey. Same rule as the bursts': no material means no effect, not a default primitive.
			Piece->DestroyComponent();
			UE_LOG(LogTraceGame, Display,
				TEXT("[FxLoop] '%s' on %s dropped: no additive material resolved (%s)."),
				NameHint, *GetNameSafe(Pawn), UTraceFxShapes::BlendName(Blend));
			return nullptr;
		}

		UTraceFxShapes::SetGlow(MID, Blend, Hue, ClampIntensity(Intensity));

		TraceFxLoopBudgetFile::Prune();
		TraceFxLoopBudgetFile::FPawnLoopEntry* Entry = TraceFxLoopBudgetFile::Find(Pawn);
		if (Entry == nullptr)
		{
			Entry = &TraceFxLoopBudgetFile::GRegistry.AddDefaulted_GetRef();
			Entry->Pawn = Pawn;
		}
		Entry->Pieces.Add(Piece);

		OutMID = MID;
		return Piece;
	}

	void DetachLoopPrimitive(APawn* Pawn, UPrimitiveComponent* Piece)
	{
		if (Piece == nullptr)
		{
			return;
		}

		TraceFxLoopBudgetFile::Prune();
		if (TraceFxLoopBudgetFile::FPawnLoopEntry* Entry = TraceFxLoopBudgetFile::Find(Pawn))
		{
			Entry->Pieces.RemoveAll([Piece](const TWeakObjectPtr<UPrimitiveComponent>& Held)
			{
				return Held.Get() == Piece;
			});
		}

		Piece->DestroyComponent();
	}

	void ForgetPawn(const APawn* Pawn)
	{
		TraceFxLoopBudgetFile::Prune();
		for (int32 Index = TraceFxLoopBudgetFile::GRegistry.Num() - 1; Index >= 0; --Index)
		{
			if (TraceFxLoopBudgetFile::GRegistry[Index].Pawn.Get() == Pawn)
			{
				TraceFxLoopBudgetFile::GRegistry.RemoveAtSwap(Index);
			}
		}
	}
}

// =================================================================================================
// FX_AUDIO_PLAN §8.8 — Trace.Fx.BurstTest and Trace.Fx.LoopBudget
//
// §8.8 asks for "Trace.Fx.BurstTest <type> (spawns each burst type at the crosshair, prints achieved
// blends)". This does that and two things more, because the acceptance for this tranche is
// "all 9 types print achieved blends, no grey primitive on a forced None" PLUS a photograph of each:
//
//   Trace.Fx.BurstTest <type>   ONE burst at the crosshair, live, animating and self-destroying.
//                               This is the §8.8 command, and it is what a human types.
//   Trace.Fx.BurstTest          THE PARADE. Stages the nine in front of the local player one at a
//                               time, each HELD at a known frame of its animation, photographs each,
//                               then arms Trace.Fx.BurstForceNone and re-spawns all nine at once to
//                               prove that a burst with no materials draws nothing at all.
//
// Everything is inside #if !UE_BUILD_SHIPPING — TraceCore's unguarded harness sprawl is the
// anti-pattern this project has already named (RESTRUCTURE A9).
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace TraceFxBurstVerify
{
	/** Where the parade stands each burst, relative to the player, and how it is photographed. */
	// 260 uu out and 130 uu down puts the camera at a 26.6 deg downward angle. MEASURED, not guessed:
	// the first capture run staged at 300/60, which is 11.3 deg, and photographed every ground ring
	// nearly edge-on — a picture of a line rather than of a ring.
	constexpr float StageDistanceUU = 260.f;
	constexpr float StageDropUU = 130.f;

	/**
	 * The frame of each animation the parade photographs, 0..1, PER TYPE.
	 *
	 * NOT 1.0 and not 0.0. At 0 nothing has grown yet; at 1 everything has faded to black and the
	 * frame would be an honest photograph of nothing. These are each type's most readable moment —
	 * grown, still lit — which is the frame a human is being asked to judge.
	 *
	 * They differ because the types do. A flash peaks early and is gone; a scatter needs time to
	 * separate — the first capture run photographed SlimeSplat and JarPop at a shared 0.35 and got a
	 * lump of overlapping spheres, because at that point the blobs had travelled a third of their
	 * distance and their own radii still touched.
	 */
	static float PhotogenicAlphaFor(ETraceFxBurstType Type)
	{
		switch (Type)
		{
		case ETraceFxBurstType::ChutBash:     return 0.30f;  // wedge at full size, lines still bright
		case ETraceFxBurstType::SpikeEmbed:   return 0.45f;  // sparks out, ring near its 26 uu
		case ETraceFxBurstType::ElleTeleport: return 0.40f;  // column widened, ring still growing
		case ETraceFxBurstType::RocketBurst:  return 0.30f;  // shell near full, spokes still lit
		case ETraceFxBurstType::QuakeHit:     return 0.35f;
		case ETraceFxBurstType::SlimeSplat:   return 0.60f;  // blobs separated
		case ETraceFxBurstType::BeeSting:     return 0.35f;
		case ETraceFxBurstType::JarPop:       return 0.60f;  // blobs separated
		case ETraceFxBurstType::GenericRing:  return 0.45f;
		default:                              return 0.40f;
		}
	}

	/** How long the parade leaves each burst up. The screenshot lands on the render thread later. */
	constexpr float StageHoldSeconds = 1.4f;

	static UWorld* FindGameWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld())
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/**
	 * Where a type is SPAWNED relative to the stage plane, uu up.
	 *
	 * Not framing — contract. ElleTeleport is documented as being spawned at the arriving pawn's
	 * CAPSULE CENTRE (that is where a gate stores its mouth), so its ground ring sits half a player
	 * BELOW the spawn point. Staged like everything else it would put its ring under the floor, and
	 * the photograph would be of a bug the harness had introduced.
	 */
	static float StageZOffsetFor(ETraceFxBurstType Type)
	{
		return (Type == ETraceFxBurstType::ElleTeleport) ? 88.f : 0.f;
	}

	static APlayerController* FindLocalPlayerController(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = Cast<APlayerController>(It->Get());
			if (PC != nullptr && PC->IsLocalController())
			{
				return PC;
			}
		}
		return nullptr;
	}

	/**
	 * The crosshair point: what the local player is looking at, up to 1,200 uu away, else a point
	 * that far in front of them. The surface normal comes back too, because six of the nine types
	 * want it (see FTraceFxBurstSpec::Direction).
	 */
	static bool ResolveCrosshair(UWorld* World, APlayerController* PC, FVector& OutLocation, FVector& OutNormal)
	{
		if (World == nullptr || PC == nullptr)
		{
			return false;
		}

		FVector ViewLocation = FVector::ZeroVector;
		FRotator ViewRotation = FRotator::ZeroRotator;
		PC->GetPlayerViewPoint(ViewLocation, ViewRotation);

		const FVector Forward = ViewRotation.Vector();
		const FVector End = ViewLocation + Forward * 1200.f;

		FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceFxBurstTest), /*bTraceComplex*/ false);
		Params.AddIgnoredActor(PC->GetPawn());

		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, ViewLocation, End, ECC_Visibility, Params))
		{
			OutLocation = Hit.ImpactPoint + Hit.ImpactNormal * 2.f;
			OutNormal = Hit.ImpactNormal;
			return true;
		}

		OutLocation = ViewLocation + Forward * 400.f;
		OutNormal = -Forward;
		return true;
	}

	/** One line per type, in the format the acceptance asks for. */
	static void ReportBurst(const ATraceFxBurst* Spawned, ETraceFxBurstType Type, const TCHAR* Note)
	{
		if (Spawned == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[FxBurst] %s: SPAWN FAILED (%s)."),
				ATraceFxBurst::TypeName(Type), Note);
			return;
		}

		const FName Event = ATraceFxBurst::SoundEventFor(Type);

		// THE HUE, ON THE SAME LINE AS EVERYTHING ELSE. It was not printed until W6, which is a large
		// part of why seven stale colour literals survived a whole wave in this file: nothing in the
		// harness ever said what colour anything came out. HueFor is the shipped path, so this is the
		// number the pieces were actually given (the parade never passes a Tint override).
		const uint8 OwnerId = TraceFxBurstFile::AccentOwnerFor(Type);
		const FLinearColor BuiltHue = ATraceFxBurst::HueFor(Type);

		UE_LOG(LogTraceGame, Display,
			TEXT("[FxBurst] %-12s %s | radius %6.1f uu | primitives %d, visible %d | %s | sound %s%s "
			     "| hue (%.3f,%.3f,%.3f) from %s"),
			ATraceFxBurst::TypeName(Type), Note,
			Spawned->GetResolvedRadiusUU(),
			Spawned->GetPrimitiveCount(), Spawned->GetVisiblePrimitiveCount(),
			*Spawned->DescribeBlends(),
			Event.IsNone() ? TEXT("<none by design>") : *Event.ToString(),
			(Event.IsNone() || Spawned->DidPlaySound()) ? TEXT("") : TEXT(" *** NOT PLAYED ***"),
			BuiltHue.R, BuiltHue.G, BuiltHue.B,
			(OwnerId == TraceCharacterRoster::NoneId)
				? TEXT("the semantic wheel")
				: *FString::Printf(TEXT("roster accent %s"), *TraceCharacterRoster::NameFor(OwnerId)));
	}

	/**
	 * Asks for a screenshot and logs it in the SAME format ATraceHUD's TraceAutoShot uses, because
	 * the frame-harvest scripts grep for that line and a second spelling of it is a frame nobody
	 * collects. The View line goes with it for the same reason it does there: a frame that looks
	 * wrong cannot be diagnosed without the camera that took it.
	 */
	static void RequestFrame(UWorld* World, APlayerController* PC, const FString& Label)
	{
		if (World == nullptr)
		{
			return;
		}

		const FString FileName = FString::Printf(TEXT("TraceAutoShot_burst_%s_%s.png"),
			*Label, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName);

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Path));

		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
		UE_LOG(LogTraceGame, Display, TEXT("[AutoShot] Screenshot requested: %s"), *Path);

		if (PC != nullptr)
		{
			FVector ViewLocation = FVector::ZeroVector;
			FRotator ViewRotation = FRotator::ZeroRotator;
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
			UE_LOG(LogTraceGame, Display,
				TEXT("[AutoShot] View: map=%s pawn=%s at %s | camera %s rot %s"),
				*World->GetMapName(), *GetNameSafe(PC->GetPawn()),
				PC->GetPawn() ? *PC->GetPawn()->GetActorLocation().ToCompactString() : TEXT("<none>"),
				*ViewLocation.ToCompactString(), *ViewRotation.ToCompactString());
		}
	}

	/** Spawns ONE burst at the crosshair, live. The §8.8 command. */
	static void RunOne(ETraceFxBurstType Type)
	{
		UWorld* World = FindGameWorld();
		APlayerController* PC = FindLocalPlayerController(World);
		if (World == nullptr || PC == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[FxBurst] no game world with a local player controller."));
			return;
		}
		if (World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[FxBurst] this is a CLIENT. A burst is spawned by the authority and arrives here by "
				     "replication; run the command on the server and watch it appear on this machine."));
			return;
		}

		FVector Where = FVector::ZeroVector;
		FVector Normal = FVector::UpVector;
		ResolveCrosshair(World, PC, Where, Normal);

		ATraceFxBurst* Spawned = ATraceFxBurst::Burst(World, Type, Where, Normal);
		ReportBurst(Spawned, Type, TEXT("live "));
	}

	/**
	 * THE PARADE. One type at a time, held at a known frame, photographed; then the forced-None pass.
	 *
	 * It drives itself off a repeating timer rather than a sleep, because a console command that
	 * blocked the game thread for twenty seconds would photograph twenty identical frozen frames.
	 */
	struct FParadeState
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<APlayerController> PC;
		TWeakObjectPtr<ATraceFxBurst> Current;
		TArray<TWeakObjectPtr<ATraceFxBurst>> NoneWave;
		FVector Stage = FVector::ZeroVector;
		FTimerHandle Timer;
		int32 Step = 0;
		int32 Failures = 0;
		int32 Checks = 0;
	};

	static FParadeState GParade;

	static void ParadeCheck(bool bCondition, const TCHAR* What, const FString& Detail)
	{
		++GParade.Checks;
		if (bCondition)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[FxBurst]   ok   %s — %s"), What, *Detail);
		}
		else
		{
			++GParade.Failures;
			UE_LOG(LogTraceGame, Error, TEXT("[FxBurst]   FAIL %s — %s"), What, *Detail);
		}
	}

	static void ParadeStep();

	static void ScheduleNextStep(float Delay)
	{
		UWorld* World = GParade.World.Get();
		if (World == nullptr)
		{
			return;
		}
		World->GetTimerManager().SetTimer(GParade.Timer,
			FTimerDelegate::CreateWeakLambda(World, []() { ParadeStep(); }), FMath::Max(0.05f, Delay), false);
	}

	static void ParadeStep()
	{
		UWorld* World = GParade.World.Get();
		if (World == nullptr)
		{
			return;
		}

		const int32 TypeCount = static_cast<int32>(ETraceFxBurstType::Count);

		// --- clean up the previous step's burst ---------------------------------------------------
		if (ATraceFxBurst* Previous = GParade.Current.Get())
		{
			Previous->Destroy();
			GParade.Current = nullptr;
		}

		// --- phase 1: the nine types, one at a time, each held and photographed --------------------
		if (GParade.Step < TypeCount)
		{
			const ETraceFxBurstType Type = static_cast<ETraceFxBurstType>(GParade.Step);

			// Directions are chosen to PRESENT each type to the lens rather than to be realistic:
			// world up puts a wedge's profile and a ring's face where the camera can see both. The
			// live command (RunOne) uses the real surface normal.
			ATraceFxBurst* Spawned = ATraceFxBurst::Burst(World, Type,
				GParade.Stage + FVector(0.f, 0.f, StageZOffsetFor(Type)), FVector::UpVector);
			if (Spawned != nullptr)
			{
				Spawned->DebugHoldAt(PhotogenicAlphaFor(Type), StageHoldSeconds + 0.6f);
			}
			GParade.Current = Spawned;

			ReportBurst(Spawned, Type, TEXT("held "));

			// The acceptance, measured per type rather than asserted once at the end.
			if (Spawned != nullptr)
			{
				ParadeCheck(Spawned->GetPrimitiveCount() > 0
					&& Spawned->GetPrimitiveCount() <= ATraceFxBurst::MaxPrimitivesPerBurst,
					TEXT("primitive budget"),
					FString::Printf(TEXT("%s built %d component(s), ceiling %d"),
						ATraceFxBurst::TypeName(Type), Spawned->GetPrimitiveCount(),
						ATraceFxBurst::MaxPrimitivesPerBurst));

				ParadeCheck(Spawned->GetVisiblePrimitiveCount() == Spawned->GetPrimitiveCount(),
					TEXT("every piece resolved a material"),
					FString::Printf(TEXT("%s: %d of %d visible — %s"),
						ATraceFxBurst::TypeName(Type), Spawned->GetVisiblePrimitiveCount(),
						Spawned->GetPrimitiveCount(), *Spawned->DescribeBlends()));

				ParadeCheck(ATraceFxBurst::AnimSecondsFor(Type) <= ATraceFxBurst::MaxAnimSeconds
					&& ATraceFxBurst::AnimSecondsFor(Type) < ATraceFxBurst::LifeSpanSeconds,
					TEXT("animation fits inside the lifespan"),
					FString::Printf(TEXT("%s: %.2f s of animation, %.2f s cap, %.2f s lifespan"),
						ATraceFxBurst::TypeName(Type), ATraceFxBurst::AnimSecondsFor(Type),
						ATraceFxBurst::MaxAnimSeconds, ATraceFxBurst::LifeSpanSeconds));

				const FName Event = ATraceFxBurst::SoundEventFor(Type);
				if (!Event.IsNone())
				{
					ParadeCheck(Spawned->DidPlaySound(), TEXT("the type's sound was played locally"),
						FString::Printf(TEXT("%s -> %s (PlayReplicatedLocal, no RPC)"),
							ATraceFxBurst::TypeName(Type), *Event.ToString()));
				}

				// *** THE CHECK THAT WOULD HAVE CAUGHT THE W5 DESYNC ON THE DAY IT HAPPENED. ***
				//
				// The body a player looks at is stamped from TraceCharacterRoster (the ten
				// MI_Body_*_Accent instances and the ten DataAssets are generated from that one
				// table), so "does the ability fire in the same hue the body wears" is answerable
				// exactly: the burst's hue must BE the owner's roster accent, to the bit. It is,
				// now, because HueFor reads that table instead of a copy of it — which makes this
				// check cheap rather than clever. Run under Trace.Fx.LegacyAccents 1 it goes red on
				// all seven accent types, which is how the check is known to be able to fail.
				const uint8 AccentOwner = TraceFxBurstFile::AccentOwnerFor(Type);
				if (AccentOwner != TraceCharacterRoster::NoneId)
				{
					const TraceCharacterRoster::FTraceCharacterEntry* const Row =
						TraceCharacterRoster::Find(AccentOwner);
					const FLinearColor Accent = (Row != nullptr) ? Row->Accent : FLinearColor::White;
					const FLinearColor Built = ATraceFxBurst::HueFor(Type);
					const bool bMatches = Row != nullptr
						&& FMath::IsNearlyEqual(Built.R, Accent.R, 1.e-4f)
						&& FMath::IsNearlyEqual(Built.G, Accent.G, 1.e-4f)
						&& FMath::IsNearlyEqual(Built.B, Accent.B, 1.e-4f);

					ParadeCheck(bMatches, TEXT("hue IS the owner's live roster accent"),
						FString::Printf(
							TEXT("%s wears %s: built (%.3f,%.3f,%.3f) vs roster (%.3f,%.3f,%.3f)%s"),
							ATraceFxBurst::TypeName(Type),
							*TraceCharacterRoster::NameFor(AccentOwner),
							Built.R, Built.G, Built.B, Accent.R, Accent.G, Accent.B,
							bMatches ? TEXT("") : TEXT("  <- the ability and the body disagree")));
				}
				else
				{
					const FLinearColor Built = ATraceFxBurst::HueFor(Type);
					UE_LOG(LogTraceGame, Display,
						TEXT("[FxBurst]   --   %s is SEMANTIC (%.3f,%.3f,%.3f) — no owner, "
						     "deliberately not a roster read."),
						ATraceFxBurst::TypeName(Type), Built.R, Built.G, Built.B);
				}
			}

			RequestFrame(World, GParade.PC.Get(), ATraceFxBurst::TypeName(Type));

			++GParade.Step;
			ScheduleNextStep(StageHoldSeconds);
			return;
		}

		// --- phase 2: the forced-None wave --------------------------------------------------------
		if (GParade.Step == TypeCount)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[FxBurst] --- forced-None pass: arming Trace.Fx.BurstForceNone 1 and spawning all "
				     "nine at once. NOTHING may appear, and every sound must still play. ---"));

			CVarFxBurstForceNone->Set(1, ECVF_SetByConsole);

			int32 Grey = 0;
			for (int32 Index = 0; Index < TypeCount; ++Index)
			{
				const ETraceFxBurstType Type = static_cast<ETraceFxBurstType>(Index);

				// Spread them across the stage so a single stray grey primitive could not hide behind
				// another one in the frame.
				const float Angle = (2.f * PI * static_cast<float>(Index)) / static_cast<float>(TypeCount);
				const FVector Spot = GParade.Stage + FVector(FMath::Cos(Angle) * 90.f, FMath::Sin(Angle) * 90.f,
					StageZOffsetFor(Type));

				ATraceFxBurst* Spawned = ATraceFxBurst::Burst(World, Type, Spot, FVector::UpVector);
				if (Spawned != nullptr)
				{
					Spawned->DebugHoldAt(PhotogenicAlphaFor(Type), StageHoldSeconds + 0.6f);
					GParade.NoneWave.Add(Spawned);
					Grey += Spawned->GetVisiblePrimitiveCount();

					const FName Event = ATraceFxBurst::SoundEventFor(Type);
					ParadeCheck(Event.IsNone() || Spawned->DidPlaySound(),
						TEXT("sound still plays with no material"),
						FString::Printf(TEXT("%s -> %s"), ATraceFxBurst::TypeName(Type),
							Event.IsNone() ? TEXT("<none by design>") : *Event.ToString()));
				}
				ReportBurst(Spawned, Type, TEXT("None "));
			}

			ParadeCheck(Grey == 0, TEXT("NO GREY PRIMITIVE on a forced None"),
				FString::Printf(TEXT("%d visible primitive(s) across all nine types with every material "
					"refused; anything above zero is engine-default grey on screen"), Grey));

			RequestFrame(World, GParade.PC.Get(), TEXT("ForcedNone"));

			++GParade.Step;
			ScheduleNextStep(StageHoldSeconds);
			return;
		}

		// The forced-None wave has been photographed; tear it down and disarm before the ladder, which
		// re-uses the same list and needs real materials.
		if (GParade.Step == TypeCount + 1 && GParade.NoneWave.Num() > 0)
		{
			for (const TWeakObjectPtr<ATraceFxBurst>& Weak : GParade.NoneWave)
			{
				if (ATraceFxBurst* Burst = Weak.Get())
				{
					Burst->Destroy();
				}
			}
			GParade.NoneWave.Reset();
			CVarFxBurstForceNone->Set(0, ECVF_SetByConsole);
		}

		// --- phase 3: THE HUE LADDER ---------------------------------------------------------------
		//
		// One type, one ring, four values of the hue headroom, four frames. This is how ATraceElleGate
		// settled its own ring glow (it photographed 3.5 / 1.4 / 1.0 and read the hue out of the
		// pixels), and it is why the number in this file is a measurement rather than a preference.
		if (GParade.Step == TypeCount + 1)
		{
			static const float Ladder[] = { 0.7f, 1.0f, 1.4f, 2.0f };

			UE_LOG(LogTraceGame, Display,
				TEXT("[FxBurst] --- hue ladder: ChutBash at Trace.Fx.BurstHueHeadroom %.1f / %.1f / %.1f "
				     "/ %.1f. Read the ring's hue off the frames; the shipping value is the one whose "
				     "ring is still MINT. ---"), Ladder[0], Ladder[1], Ladder[2], Ladder[3]);

			for (const float Headroom : Ladder)
			{
				CVarFxBurstHueHeadroom->Set(Headroom, ECVF_SetByConsole);
				ATraceFxBurst* Rung = ATraceFxBurst::Burst(World, ETraceFxBurstType::ChutBash,
					GParade.Stage, FVector::UpVector);
				if (Rung != nullptr)
				{
					// ALL FOUR ARE SPAWNED AT ONCE and photographed one per step, so each has to
					// outlive the WHOLE ladder, not one hold. The first attempt gave them one hold
					// each and the last two rungs had already destroyed themselves by the time their
					// frames were taken — two empty photographs presented as a measurement.
					Rung->DebugHoldAt(PhotogenicAlphaFor(ETraceFxBurstType::ChutBash),
						StageHoldSeconds * (UE_ARRAY_COUNT(Ladder) + 2));
					GParade.NoneWave.Add(Rung);
				}
			}
			CVarFxBurstHueHeadroom->Set(TraceFxBurstFile::EmissiveHueHeadroom, ECVF_SetByConsole);

			// They are stacked on one spot, so they are photographed one at a time: hide all, then
			// show one per frame. Cheaper than four more timer steps and the frames are identical
			// except for the one variable, which is what makes a ladder a ladder.
			ScheduleNextStep(0.2f);
			++GParade.Step;
			return;
		}

		if (GParade.Step > TypeCount + 1 && GParade.Step <= TypeCount + 5)
		{
			const int32 Rung = GParade.Step - (TypeCount + 2);
			static const TCHAR* RungNames[] = { TEXT("HueLadder_0p7"), TEXT("HueLadder_1p0"),
				TEXT("HueLadder_1p4"), TEXT("HueLadder_2p0") };

			for (int32 Index = 0; Index < GParade.NoneWave.Num(); ++Index)
			{
				if (ATraceFxBurst* Burst = GParade.NoneWave[Index].Get())
				{
					Burst->SetActorHiddenInGame(Index != Rung);
				}
			}
			RequestFrame(World, GParade.PC.Get(), RungNames[FMath::Clamp(Rung, 0, 3)]);

			++GParade.Step;
			ScheduleNextStep(StageHoldSeconds);
			return;
		}

		// --- phase 4: disarm and report -----------------------------------------------------------
		for (const TWeakObjectPtr<ATraceFxBurst>& Weak : GParade.NoneWave)
		{
			if (ATraceFxBurst* Burst = Weak.Get())
			{
				Burst->Destroy();
			}
		}
		GParade.NoneWave.Reset();

		CVarFxBurstForceNone->Set(0, ECVF_SetByConsole);

		if (GParade.Failures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[FxBurst] VERDICT: PASS — %d checks, 0 failures across all %d burst types."),
				GParade.Checks, TypeCount);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[FxBurst] VERDICT: *** %d PROBLEM(S) *** of %d checks."), GParade.Failures, GParade.Checks);
		}
	}

	static void RunParade()
	{
		UWorld* World = FindGameWorld();
		APlayerController* PC = FindLocalPlayerController(World);
		if (World == nullptr || PC == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[FxBurst] no game world with a local player controller."));
			return;
		}
		if (World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[FxBurst] the parade must run on the authority."));
			return;
		}

		// Stage the bursts a short way in front of the player and LOOK AT THEM. Without the second
		// half a ground ring is photographed edge-on, which is a picture of a line.
		FVector ViewLocation = FVector::ZeroVector;
		FRotator ViewRotation = FRotator::ZeroRotator;
		PC->GetPlayerViewPoint(ViewLocation, ViewRotation);

		const FVector Forward = FVector(ViewRotation.Vector().X, ViewRotation.Vector().Y, 0.f).GetSafeNormal();
		const FVector Stage = ViewLocation + Forward * StageDistanceUU - FVector(0.f, 0.f, StageDropUU);

		PC->SetControlRotation((Stage - ViewLocation).Rotation());

		GParade = FParadeState();
		GParade.World = World;
		GParade.PC = PC;
		GParade.Stage = Stage;

		UE_LOG(LogTraceGame, Display,
			TEXT("[FxBurst] ===== BurstTest parade: %d types, staged at %s, each held at its own "
			     "photogenic frame, %.1f s each. Frames land in Saved/Screenshots as "
			     "TraceAutoShot_burst_<Type>_*.png. ====="),
			static_cast<int32>(ETraceFxBurstType::Count), *Stage.ToCompactString(), StageHoldSeconds);

		// One frame of delay before the first spawn, so the control rotation this just set has been
		// applied to the camera before anything is photographed.
		ScheduleNextStep(0.4f);
	}

	static void RunBurstTest(const TArray<FString>& Args)
	{
		if (Args.Num() == 0 || Args[0].Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			RunParade();
			return;
		}

		ETraceFxBurstType Type = ETraceFxBurstType::GenericRing;
		if (!ATraceFxBurst::ParseType(Args[0], Type))
		{
			FString Known;
			for (int32 Index = 0; Index < static_cast<int32>(ETraceFxBurstType::Count); ++Index)
			{
				Known += FString::Printf(TEXT("%s%s"), (Index > 0) ? TEXT(", ") : TEXT(""),
					ATraceFxBurst::TypeName(static_cast<ETraceFxBurstType>(Index)));
			}
			UE_LOG(LogTraceGame, Error,
				TEXT("[FxBurst] '%s' is not a burst type. Known types: %s. No argument runs all nine."),
				*Args[0], *Known);
			return;
		}

		RunOne(Type);
	}

	static void RunLoopBudgetReport()
	{
		UWorld* World = FindGameWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[FxLoop] no game world."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[FxLoop] ===== attached-loop FX budget (§1.4: max %d primitives per pawn, additive "
			     "only, intensity <= %.2f, <= %.0f uu from the capsule axis) ====="),
			TraceFxLoopBudget::MaxPrimitivesPerPawn, TraceFxLoopBudget::MaxIntensity,
			TraceFxLoopBudget::MaxOffsetFromAxisUU);

		int32 Pawns = 0;
		int32 Over = 0;
		for (TActorIterator<APawn> It(World); It; ++It)
		{
			APawn* Pawn = *It;
			const int32 Count = TraceFxLoopBudget::CountFor(Pawn);
			if (Count <= 0)
			{
				continue;
			}
			++Pawns;
			if (Count > TraceFxLoopBudget::MaxPrimitivesPerPawn)
			{
				++Over;
			}
			UE_LOG(LogTraceGame, Display, TEXT("[FxLoop]   %-32s %d loop primitive(s)%s"),
				*GetNameSafe(Pawn), Count,
				(Count > TraceFxLoopBudget::MaxPrimitivesPerPawn) ? TEXT("  *** OVER BUDGET ***") : TEXT(""));
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[FxLoop] %d pawn(s) carrying attached loop FX, %d over budget. (Zero is the correct "
			     "answer until the per-kit tranches land; the helper ships before its callers.)"),
			Pawns, Over);
	}

	FAutoConsoleCommand CmdFxBurstTest(
		TEXT("Trace.Fx.BurstTest"),
		TEXT("Dev only, SERVER. FX plan §1.3/§8.8. With a type name (ChutBash, SpikeEmbed, ElleTeleport, "
		     "RocketBurst, QuakeHit, SlimeSplat, BeeSting, JarPop, GenericRing) spawns that burst at the "
		     "crosshair, live, and prints its achieved blends. With NO argument it runs the parade: all "
		     "nine staged in front of the local player, each held at a known frame and photographed, then "
		     "a forced-None pass proving that a burst with no materials draws nothing at all."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunBurstTest));

	FAutoConsoleCommand CmdFxLoopBudget(
		TEXT("Trace.Fx.LoopBudget"),
		TEXT("Dev only. FX plan §1.4: prints how many attached loop FX primitives every pawn is carrying "
		     "and flags anything over the four-per-pawn ceiling. Zero everywhere is correct until the "
		     "per-kit tranches wire their loops."),
		FConsoleCommandDelegate::CreateStatic(&RunLoopBudgetReport));
}

#endif // !UE_BUILD_SHIPPING
