// Copyright (c) Trace. All Rights Reserved.

#include "World/TraceArenaBuilder.h"

#include "Trace.h"
#include "TraceTypes.h"
#include "TraceSettings.h"              // GoalWidthFieldFraction / GoalHeightUU (spec v4 section 7, resized v5 section 4)
#include "Core/TraceCharacter.h"        // PlayerHeightUU() reads the capsule off the CDO
#include "Core/TraceGameState.h"        // GetScoringModeFor - the one authority on which mode runs
#include "Gameplay/TraceEndzone.h"
#include "World/TraceTeamPlayerStart.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"   // USkyAtmosphereComponent AND ASkyAtmosphere
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/EngineTypes.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"                       // FPostProcessSettings
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                       // TActorIterator
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"            // NewObject, MakeUniqueObjectName

#if WITH_EDITOR
#include "UObject/UnrealType.h"                // FPropertyChangedEvent, EPropertyChangeType
#endif

namespace TraceArenaConstants
{
	/** The engine basic shapes are 100 uu across and centred on their pivot. */
	static constexpr float ShapeUnit = 100.f;

	// --- Vertical layout -------------------------------------------------------------------------

	/**
	 * Riser height of every walkable step in the arena.
	 *
	 * HARD CEILING: UCharacterMovementComponent::MaxStepHeight, engine default 45, which this project
	 * does not override (grep MaxStepHeight - it appears nowhere in Source/). Go over it and every
	 * stepped platform silently becomes an unclimbable wall, which on the centre dais would make the
	 * Core unreachable. 40 leaves a 5 uu margin.
	 */
	static constexpr float StepRise = 40.f;

	/**
	 * Fallback player height, used only if ATraceCharacter's CDO cannot be reached.
	 *
	 * The capsule is 88 uu half height, so one player height is 176 uu and the sketch's three
	 * structure classes (spec v3 section 7) are 176 / 352 / 616. The live value comes from
	 * ATraceArenaBuilder::PlayerHeightUU(), which reads the capsule; this constant exists so that a
	 * missing CDO degrades to the documented arena instead of to a field of zero-height blocks.
	 */
	static constexpr float FallbackPlayerHeight = 176.f;

	/** Structure heights from the sketch key, as multiples of one player height. */
	static constexpr float StructureHeight1x = 1.0f;    // green outline
	static constexpr float StructureHeight2x = 2.0f;    // orange outline
	static constexpr float StructureHeight35x = 3.5f;   // red outline

	/**
	 * Tiers in the centre diamond. Its top surface is therefore at StepRise * DaisTiers.
	 *
	 * FOUR TIERS AT 1500/700 -> THREE AT 900/500. The sketch draws a SMALL diamond at the exact
	 * centre; the old dais was a 3600 uu wide four-tier ziggurat, which on a 9600 uu wide field
	 * would have been more than a third of the width and would have made the centre a fortress
	 * rather than a landmark. 1900 uu across the base, 120 uu tall: high enough to be the visible
	 * pedestal for the Core, small enough to run round.
	 */
	static constexpr int32 DaisTiers = 3;
	static constexpr float DaisTopTierSide = 900.f;
	static constexpr float DaisTierSideStep = 500.f;

	/** The Core pedestal, measured from the dais top. */
	static constexpr float PedestalHeight = 60.f;
	static constexpr float PedestalDiameter = 300.f;

	/**
	 * Core rest height above the pedestal top. It falls the last few uu onto the pedestal.
	 * ATraceCore::HomeToleranceSq is 75^2, so the Core's resting position must stay inside 75 uu of
	 * GetCoreSpawnLocation() or the Core would consider itself permanently away from home.
	 */
	static constexpr float CoreDropHeight = 90.f;

	// --- Floor decals ----------------------------------------------------------------------------
	//
	// Thin slabs laid on the floor, each at its own Z so they never z-fight each other or the slab.
	static constexpr float GridZ = 2.f;
	static constexpr float GridThickness = 5.f;
	static constexpr float PadZ = 3.f;
	static constexpr float PatchZ = 4.f;
	static constexpr float PatchThickness = 8.f;
	static constexpr float GoalLineZ = 7.f;
	static constexpr float GoalLineThickness = 14.f;
	static constexpr float GoalLineWidth = 44.f;

	// --- Neon trim geometry ----------------------------------------------------------------------

	/** Bright strip running along the top inner edge of each wall - the Tron read from a distance. */
	static constexpr float WallTrimSize = 46.f;

	// --- POINT-BLANK STANDOFF --------------------------------------------------------------------
	//
	// THE DEFECT THIS FIXES, precisely. Every neon strip in this file used to be built PROUD of the
	// collision box it decorates: AddNeonBlock's trim at Size + Out*2 against a collision box at
	// Size, AddPylon's face strips at Side*0.5 + StripDepth*0.5 - 6 against a box at Side, the wall
	// ribs sitting entirely INSIDE the field in front of a wall whose collision face is the wall
	// itself. The pawn capsule is 34 uu in radius, so the first-person eye stops 34 uu from a
	// COLLISION face - and the emissive was 10-36 uu closer than that. Measured on the worst case,
	// a wall rib: the rib spanned the 36 uu immediately in front of the wall, so an eye pressed
	// against the wall was 2 uu INSIDE a 90 uu wide unlit-emissive slab. Unlit emissive is
	// distance-invariant, so it arrives at full intensity however close it is: 5% of walking frames
	// came back over 40% blown out and one was 99%.
	//
	// The cure is standoff and solid angle, NOT less glow - the face-trim brightness is a measured
	// readability win and dimming it would undo that. Two rules, applied everywhere:
	//
	//   1. NOTHING EMISSIVE MAY LIE OUTSIDE A PAWN BARRIER. Every colliding structure that carries
	//      neon at reachable height (below ~400 uu, i.e. eye height 152 plus a 90 uu jump apex plus
	//      margin) gets a PAWN-ONLY standoff shell - a box that blocks ECC_Pawn and nothing else,
	//      inflated horizontally by NeonStandoff. Pawn-only is the important part: the real
	//      collision box still governs hitscan (ECC_Visibility), bot line of sight and the spring
	//      arm probe, so peeking, shooting and the camera behave EXACTLY as they did. Only how
	//      close a body may press changes.
	//   2. NO NEON STRIP IS WIDER THAN MaxNeonStripWidth. Standoff alone cannot save a 109 uu wide
	//      buttress strip: even flush and 60 uu away it subtends 84 deg, most of a 95 deg frame.
	//      Narrower strips are also simply better Tron - a wide glowing panel reads as a light box,
	//      a thin one reads as neon - and where a face is wide enough to need more presence it now
	//      gets TWO thin strips with a dark gap rather than one fat one.
	//
	// The arithmetic these numbers come from. An eye can approach to
	//     d = PawnCapsuleRadius + NeonStandoff
	// of a flush emissive surface of width w, which subtends
	//     theta = 2 * atan(w / 2d)
	// Target: no single emissive surface over ~35% of the 95 deg horizontal field, i.e. theta <= 33
	// deg, i.e. w <= 0.59 * d. At d = 60 that is w <= 35. Hence MaxNeonStripWidth = 36 and the
	// narrowed rib sizes below. Worst case after this pass is the corner rib at 31 deg (33% of the
	// frame) - down from 116 deg, and no longer reachable from inside the geometry at all.
	static constexpr float PawnCapsuleRadius = 34.f;

	/**
	 * Horizontal inflation of the pawn-only shell around interior structure.
	 *
	 * Also has to be at least as large as the furthest any trim protrudes from the body it
	 * decorates, or the shell would not actually enclose the thing it is protecting the player
	 * from: corner ribs are centred ON the corner and protrude CornerRibSide * 0.5 = 13, the lip
	 * protrudes LipOut = 12, the skirt SkirtOut = 10. 26 clears all three and leaves 13 uu of
	 * genuine clearance on top of the capsule radius.
	 */
	static constexpr float NeonStandoff = 26.f;

	/**
	 * The same idea on the perimeter walls, and larger, because a wall rib has nowhere to hide: a
	 * rib flush with a flat wall still has to protrude to be seen at all, and the walls are the one
	 * surface a player slides along for seconds at a time. 40 puts the eye 74 uu off the wall face
	 * and 62 uu off the rib, which lands a 36 uu rib at 33 deg.
	 */
	static constexpr float WallNeonStandoff = 40.f;

	/** See the solid-angle arithmetic above. Applies to every full-height neon strip in the arena. */
	static constexpr float MaxNeonStripWidth = 36.f;

	/**
	 * Vertical ribs and the horizontal mid-band that break up 2600 uu of otherwise blank wall.
	 *
	 * Width 90 -> 22 and depth 36 -> 12: at the old size a rib was a slab you could stand inside
	 * (see the standoff note above). It is also a better line - 22 uu on a 24000 uu wall is a neon
	 * accent, 90 uu was a pilaster with no body behind it.
	 *
	 * 36 was the first attempt and it was MEASURED as still the arena's worst remaining frame: a
	 * capture taken walking the -Y flank came back with the whole left third solid clipped cyan,
	 * 34.7% of the frame, which is the 32 degrees a 36 uu rib subtends at the 62 uu the standoff
	 * leaves. Widening the standoff to fix that would have been a 60 uu invisible wall, so the width
	 * came down instead: 22 uu subtends 20 degrees, 17% of the frame, and a wall rib is not a piece
	 * of cover whose bulk anyone needs to judge - it is a tick mark measuring your speed along a
	 * wall, and a tick mark should be thin.
	 */
	static constexpr float WallRibWidth = 22.f;
	static constexpr float WallRibDepth = 12.f;

	/**
	 * 3000 -> 2200, to buy back the rhythm the narrower rib costs.
	 *
	 * The ribs exist to give 24000 uu of wall a beat you can judge your own speed against, and that
	 * beat is a function of how OFTEN a rib passes, not how fat each one is. Halving the width and
	 * leaving the spacing alone would have traded a whiteout for a bare wall; closing the spacing up
	 * keeps the same number of lines crossing the eye per second while each one is a line rather
	 * than a slab. Eleven per side wall instead of nine, i.e. sixteen extra components in total.
	 */
	static constexpr float WallRibSpacing = 2200.f;
	static constexpr float WallBandHeightFraction = 0.42f;
	static constexpr float WallBandSize = 30.f;

	/**
	 * A second horizontal band low on the wall - a kick rail.
	 *
	 * MEASURED, in first person. The existing band sits at 42% of 2600 uu, i.e. 1092 uu up, which is
	 * seven times a player's eyeline: standing anywhere near a side wall you are looking at bare wall
	 * with the nearest feature far overhead and the nearest vertical rib up to 1500 uu along it. One
	 * screenshot of a player pressed into the wall is a black frame. At 13% (338 uu) this band is a
	 * couple of head-heights up, so it is in frame at conversational distance and it gives the wall a
	 * ground plane to sit on rather than floating out of the dark.
	 */
	static constexpr float WallKickBandHeightFraction = 0.13f;

	/**
	 * The glowing lip AddNeonBlock wraps around the top of every structural block.
	 * LipDrop + LipHeight must stay under StepRise or a dais tier's lip would stick out above the
	 * tier it belongs to.
	 */
	static constexpr float LipOut = 12.f;
	static constexpr float LipHeight = 18.f;
	static constexpr float LipDrop = 8.f;

	// --- Face trim -------------------------------------------------------------------------------
	//
	// See the AddNeonBlock comment in the header for why a top lip on its own was not enough. These
	// are the numbers behind it.

	/**
	 * Blocks shorter than this get the lip only.
	 *
	 * The 40 uu risers of a stepped platform and the low floor decals do not have a vertical face
	 * worth lighting, and trimming them would triple the component count of the dais for nothing.
	 * 240 -> 160, because the SHORTEST COVER IN THE ARENA IS NOW 176 uu.
	 *
	 * Spec v3 section 7 keys structure heights to the player, and its lowest class - a green
	 * outline - is exactly one player height. At 240 those blocks would have fallen through this
	 * test and got the top lip alone, which is the precise failure the face trim was added to fix
	 * (a black slab seen head-on), and they would also have missed the pawn-only standoff shell
	 * AddNeonBlock hangs off the same test - so a first-person eye could have pressed to 34 uu of a
	 * lip protruding 12 uu into the field, which is the point-blank whiteout coming back.
	 *
	 * 160 still sits above the 120 uu centre diamond and above any terrace of a corner bank (those
	 * pass bFaceBands=false anyway), so nothing walkable gains trim it should not have.
	 */
	static constexpr float FaceTrimMinHeight = 160.f;

	/** Glowing skirt where a block meets the floor. Its reflection in the floor doubles the read. */
	static constexpr float SkirtOut = 10.f;
	static constexpr float SkirtHeight = 24.f;
	static constexpr float SkirtRise = 14.f;

	/** Horizontal band around the middle of a face. Thin: it is there to give the face a scale. */
	static constexpr float BandOut = 5.f;
	static constexpr float BandHeight = 15.f;

	/**
	 * Vertical rib on each of the four corners, centred ON the corner so it reads as a lit edge
	 * rather than as a stripe painted near one. This is the piece that gives a block a silhouette
	 * from head on, which is the whole point.
	 *
	 * 42 -> 26. It stays centred on the corner - that is what makes it read as a lit EDGE from
	 * every angle, and moving it inboard would turn it into a stripe painted near a corner. What
	 * changed is its width, for the solid-angle reason in the standoff note above: at the closest
	 * an eye can now get (60 uu to the body face, 47 uu to the rib's outer surface) a 26 uu rib
	 * subtends 31 deg, a 42 uu one subtended 49 deg. Its 13 uu protrusion is also now comfortably
	 * inside NeonStandoff, which is what makes the shell actually enclose it.
	 */
	static constexpr float CornerRibSide = 26.f;
	static constexpr float CornerRibClearance = 10.f;

	/**
	 * A second, thinner rib down the CENTRE of each of the four faces.
	 *
	 * MEASURED, and the reason this exists at all: a screenshot taken with the camera jammed against
	 * a cover block came back as a black frame with one bright line down each edge and nothing in
	 * between. That is the corner ribs working exactly as designed - and still not enough, because at
	 * point-blank range a 900 uu face fills the whole frame, both horizontal bands are outside the
	 * vertical field of view, and the corners are at the very edges of it. A line down the middle of
	 * the face is the one piece of trim that is guaranteed to be in frame no matter how close you
	 * are, which is precisely the case the original defect report described.
	 *
	 * 30 -> 20, same solid-angle reasoning as CornerRibSide. This is the rib that is GUARANTEED to
	 * be in frame at point-blank range, so it is the one that most needs to be a line rather than a
	 * panel: 20 uu at the 50 uu closest approach is 23 deg, a quarter of the frame, with the dark
	 * face of the block either side of it doing the rest of the work.
	 */
	static constexpr float FaceRibSide = 20.f;

	// --- Spawns ----------------------------------------------------------------------------------

	/**
	 * Height of a spawn pad above the floor top (Z = 0).
	 *
	 * The number that matters is ATraceCharacter's capsule half height (88 uu), NOT the player start
	 * capsule's — the pawn is spawned centred on this Z, so anything below 88 buries it in the floor
	 * slab. 140 leaves ~52 uu of clearance so the pawn drops onto the floor instead of starting
	 * interpenetrating it.
	 */
	static constexpr float PlayerStartZ = 140.f;

	/** How far the spawn line sits in front of a team's own goal line, as a fraction of half length. */
	static constexpr float StartInsetFraction = 0.10f;

	/**
	 * Fraction of the half width the spawn fan covers.
	 *
	 * 0.7 -> 0.6. On a 9600 uu field 0.7 puts the outermost pad at Y = 3360, which is 60 uu inside
	 * the corner banks (they start at HalfWidth - BankDepth = 3300): a pawn would spawn standing on
	 * the first terrace with its capsule half in the riser. 0.6 lands the fan at +/-2880, i.e. 420
	 * uu clear of the bank, and the five pads 1440 uu apart. Re-check this against BankDepth if you
	 * change either.
	 */
	static constexpr float StartSpreadFraction = 0.6f;

	// --- Palette ---------------------------------------------------------------------------------
	//
	// Two rules, and the whole art direction falls out of them:
	//
	//  1. STRUCTURE IS ALMOST BLACK. These are BaseColor values on a lit material and they sit
	//     between 0.004 and 0.024 - darker than anything real (charcoal is ~0.04). That is
	//     deliberate: this is a computer, not a room. The previous build used 0.026-0.10 under a
	//     daylight sky and rendered as a light-grey box, which is the exact opposite of Tron.
	//     Roughness is low so the floor MIRRORS the neon via screen-space reflections - that
	//     reflection, not the albedo, is what you actually see on the ground.
	//  2. EVERY EDGE IS UNLIT NEON. Anything that defines a silhouette is a separate thin block on
	//     M_TraceNeon at Glow > 1, so it clears the bloom threshold and reads as a glowing tube.
	//
	// Neon colours are near-saturated and are multiplied by their Glow at the material, so the
	// values here stay in 0..1 and the brightness lives in one place.
	//
	// MEASURED, over three screenshot passes - the two ways this goes wrong are both easy to hit:
	//
	//   albedo 0.004-0.020, key 3.2 lux ..... pure black with neon outlines floating in it. Stylish
	//                                         for one frame, unplayable: nothing had readable volume
	//                                         and a solid cover block looked like a painted line.
	//   albedo 0.030-0.085, key 9.0 lux ..... the reverse failure, and a subtle one - the VERTICAL
	//                                         faces looked right while every up-facing surface
	//                                         (platform tops, dais tiers) blew out to pale grey at
	//                                         ~200/255. The tonemapper's toe plus sRGB encoding is
	//                                         steeply non-linear down here, so a 2.7x albedo change
	//                                         is far more than a 2.7x change on screen. Judge these
	//                                         from a screenshot of a PLATFORM TOP, never from a wall.
	//
	// What is here now: dark albedos with the light doing the work (key 5.5 + fill 4.0 + bounce 2.2),
	// which lands structure around 60-90/255 - clearly solid, clearly not grey.
	static const FLinearColor FloorColor(0.0110f, 0.0135f, 0.0190f);
	static const FLinearColor WallColor(0.0190f, 0.0230f, 0.0320f);
	static const FLinearColor StructureColor(0.0155f, 0.0185f, 0.0250f);
	static const FLinearColor PedestalColor(0.0140f, 0.0170f, 0.0240f);

	/** Neutral neon: the cyan-white the neutral half of the arena is trimmed in. */
	static const FLinearColor NeonNeutral(0.18f, 0.78f, 1.00f);
	static const FLinearColor NeonNeutralPale(0.55f, 0.92f, 1.00f);

	// Glow multipliers, i.e. how far past white a strip sits and therefore how hard it blooms.
	static constexpr float GlowGrid = 1.5f;
	static constexpr float GlowCentreLine = 4.0f;
	static constexpr float GlowLip = 3.2f;
	static constexpr float GlowTrim = 4.5f;
	static constexpr float GlowRib = 2.2f;
	static constexpr float GlowPylon = 3.8f;
	static constexpr float GlowGoalLine = 6.0f;
	static constexpr float GlowRing = 2.6f;

	/**
	 * Face trim (skirt / band / corner ribs) sits BELOW the top lip on purpose.
	 *
	 * The lip is the line that tells you where a block ends and the air begins, and it has to stay
	 * the dominant one or a cover block turns into a glowing cage with no readable top edge. 1.7
	 * against GlowLip's 3.2 is roughly half the brightness before bloom, which lands the face trim
	 * clearly visible but plainly secondary.
	 */
	static constexpr float GlowFace = 1.7f;

	/** The high rail carried on the wall buttresses, and the light bridges out to the lane pylons. */
	static constexpr float GlowBridge = 3.0f;

	/** Outer-lane floor stripes. Brighter than the grid; dimmer than a goal line. */
	static constexpr float GlowLaneStripe = 3.4f;

	/** Scales a team colour down for large floor areas; full strength is reserved for thin lines. */
	static FLinearColor Dim(const FLinearColor& Color, float Scale)
	{
		return FLinearColor(Color.R * Scale, Color.G * Scale, Color.B * Scale, 1.f);
	}

	// --- Corner banks ------------------------------------------------------------------------------
	//
	// THE SKETCH'S GREEN ARROWS. Four terraced banks, one per quadrant, raised along the long edges
	// and stepping DOWN toward the middle of the field: a shallow stadium bowl with a flat central
	// playfield. The geometry is built in BuildCornerBanks; these are the shape parameters.
	//
	// Each bank is a nest of solid boxes rising from Z = 0, exactly like AddSteppedPlatform, so
	// there are no seams for a capsule to catch on and every riser is identical. Terrace k is
	// strictly inside terrace k-1 in BOTH axes, which is what makes the top surface a staircase
	// descending toward the centre and toward the goal line rather than a cliff at either end.
	//
	// WHY TERRACED AND NOT A RAMP. Trace's bots have no navmesh: they steer straight at their
	// target with AddMovementInput (see ATraceBotController). A pitched ramp needs a rotated
	// collision box, which is a different primitive from everything else here, and a bot that meets
	// its downhill EDGE side-on gets a wall. Terraces are the same axis-aligned boxes the rest of
	// the arena is made of, and UCharacterMovementComponent steps up anything under MaxStepHeight
	// without the mover ever knowing there was a step - so a bot walking flat out at a target on the
	// far side of a bank simply walks up and over it.

	/**
	 * Fraction of the half length the bank reaches IN from the goal line, toward midfield, at its
	 * top terrace. The lowest terrace always runs the full half length.
	 *
	 * This is what makes the bowl a bowl instead of a trough: the terraces get shorter as they get
	 * higher, so the bank is highest at the corner and tapers away to a single 39 uu lip by the
	 * halfway line. 0.62 of 12000 puts the top terrace's inboard end at X = 5952, i.e. the bank
	 * climbs over 5952 uu of run - about 3.4 degrees - which is a landscape, not an obstacle.
	 */
	static constexpr float BankInboardTaperFrac = 0.62f;

	/**
	 * How far the top terrace stops SHORT of the goal line, in uu, so the bank steps back down into
	 * a flat endzone instead of ending in a wall.
	 *
	 * The endzones stay flat and full width on purpose: the goal line, the endzone floor patch, the
	 * gate towers, the spawn pads and the endzone respawn pads all live in there, and every one of
	 * them assumes a flat floor at Z = 0. 1400 uu of run over the bank's full height is a ~14 degree
	 * descent, comfortably walkable, and it puts the high ground where a defender wants it - looking
	 * back over their own goal line.
	 */
	static constexpr float BankGoalSetback = 1400.f;

	/**
	 * The lowest terrace stops this far short of the goal line too, so it cannot bury the goal line
	 * decal. GoalLineWidth is 44 uu centred on the line, so anything over 22 clears it; 60 leaves a
	 * visible dark gutter between the bank's toe and the lit line.
	 */
	static constexpr float BankGoalClearance = 60.f;

	// --- Interior layout -------------------------------------------------------------------------
	//
	// TWO ANCHORS, AND THE 3.5:1 LENGTHENING IS WHY.
	//
	// Everything used to be a FRACTION of HalfLength() / HalfWidth(). That rescales coherently, and
	// for the corner banks, the flanks and the lighting it is still exactly right. It is WRONG for
	// the cover at the business end of the field, and the 24000 -> 33600 pass is what exposed it:
	// the spawn pads sit a fraction of the half length IN FRONT OF THE GOAL LINE (see
	// StartInsetFraction), so as the field grows the pad line and a length-fraction cover block move
	// at different rates and slide through each other. Checked on the new field: the old block D at
	// 0.6083 would have landed at X = 10215 against a pad at X = 12720... and the old block E at
	// 0.75 would have landed at (12600, 2100) with a pad at (12720, 1440) INSIDE its 45-degree
	// footprint. A pawn spawned inside solid geometry, silently, from a change of field length.
	//
	// So the scatter is now two lists with two anchors:
	//
	//   ApproachCover  measured in uu BACK FROM THE GOAL LINE. These are the pieces whose job is
	//                  defined relative to the goal - the tower a defender fights around, the block
	//                  a carrier has to beat - and their distances to the goal line, to the gate and
	//                  to the spawn fan are the tuned numbers worth preserving. They do not move
	//                  when the field is lengthened; the field grows BEHIND them.
	//   MidfieldCover  a fraction of the span from the centre line to the innermost approach block.
	//                  These are the pieces that fill the middle, and they are exactly what has to
	//                  stretch when the field does - otherwise "lengthen the map" produces two
	//                  crowded ends with an empty middle, which is the failure this section of the
	//                  spec explicitly warns about.
	//
	// The absolute numbers in the comments are for the 33600 x 9600 field: HalfLength 16800,
	// HalfWidth 4800, goal line at X = 14400, spawn fan at X = 12720 (Y = 0 / +/-1440 / +/-2880).
	//
	// The design, laid out for ONE half and mirrored through the centre line (see the header note on
	// why the sketch's asymmetry is deliberately not reproduced):
	//   |Y| < 3300     the flat central playfield, where all the cover lives
	//   |Y| > 3300     the corner banks - terrain, not cover
	// The cover scatter is placed so that no straight line from a spawn pad to the centre diamond,
	// or from the centre to either goal, is clear for its whole length, and so that the two 3.5x
	// landmarks (the top-centre tower and the goal-approach tower) are visible from most of the
	// field. Every block below is one of the sketch's three height classes and nothing in between.
	//
	// EVERY POSITION HERE WAS RE-CHECKED on the 33600 field, in the (+X, +Y) quadrant, against every
	// other entry, against the centre diamond and its pylons, both lane pylons, the corner banks'
	// inner edge (|Y| = 3300), the goal line, the mode-B goal posts at (14400, +/-1600) and the five
	// spawn pads - and checked against the PAWN STANDOFF SHELLS, not the visible boxes. The shells
	// are 26 uu proud on every side, so two blocks that look 120 uu apart leave a 68 uu channel,
	// which is exactly one capsule diameter: a gap measured off the meshes is a gap that does not
	// exist.
	//
	// The tightest shell-to-shell clearances after that check, so you know what you are working with
	// if you move one:
	//
	//   C vs the bank toe .... 124 uu   (and the toe is a 39 uu step, so it is walkable, not a wall)
	//   F vs AxisCover 1  .... 391 uu
	//   G vs AxisCover 2  .... 549 uu
	//   E vs a goal post  .... 453 uu
	//   D vs the pad line .... 890 uu
	//
	// A block overlapping a spawn pad is a pawn spawned inside solid geometry; a block overlapping
	// the bank is a block with its bottom step swallowed; a channel under 68 uu is a pocket a
	// navmesh-less bot grinds in until stuck-evade fires. All three are silent.

	struct FCoverSpec
	{
		/** ApproachCover: uu back from the goal line. MidfieldCover: fraction of the midfield span. */
		float XAnchor;
		float YFrac;        // of HalfWidth
		float SizeX;        // uu, before any rotation
		float SizeY;        // uu
		float HeightMult;   // multiple of one player height - 1x, 2x or 3.5x, per the sketch key
		float Yaw;          // 45 for a diamond, 0 for an axis-aligned bar
	};

	/**
	 * The goal approach, measured BACK FROM THE GOAL LINE. Mirrored into all four quadrants.
	 *
	 * Ordered outermost-first, and that matters: MidfieldSpan is taken from the LAST entry, which is
	 * therefore the innermost approach block and the boundary between the two lists.
	 */
	static const FCoverSpec ApproachCover[] =
	{
		// E - the 3.5x landmark tower of the goal approach. Visible from the far half; the piece a
		//     defender fights around. Its Y span deliberately falls BETWEEN two spawn-pad rows, and
		//     1000 uu back leaves its 45-degree footprint 505 uu clear of the goal line, which is
		//     what keeps it out of the mode-B goal posts.
		{ 1000.f, 0.4375f,  700.f,  700.f, StructureHeight35x, 45.f },   // (13800, 2100)
		// D - low diamond on the endzone approach, 1x: shootable over, not hideable behind.
		{ 3400.f, 0.2708f, 1100.f, 1100.f, StructureHeight1x,  45.f },   // (11000, 1300)
		// C - upright bar across the lane, 2x. Reads as a gate between the spine and the flank, and
		//     it is the innermost approach block, so it also defines where the midfield ends.
		{ 4800.f, 0.5208f,  400.f, 1300.f, StructureHeight2x,   0.f }    // (9600, 2500)
	};

	/**
	 * The midfield, as a fraction of the span from the centre line to the innermost ApproachCover
	 * block. Mirrored into all four quadrants.
	 *
	 * THIS IS THE LIST THAT ANSWERS "adjusting the structures to match". The span it divides went
	 * from 6000 uu to 9600 uu with the lengthening, so it gained a third entry: two blocks stretched
	 * over the longer midfield would have left a 3900 uu hole where the old field had 2350, and a
	 * hole that size at eye height is not "open space", it is a corridor with nothing in it.
	 */
	static const FCoverSpec MidfieldCover[] =
	{
		// A - long low bar. The sketch's "long low bar", and the piece that stops the run from the
		//     centre diamond out into the half being a straight line.
		{ 0.27f, 0.4792f, 2600.f,  400.f, StructureHeight1x,  0.f },   // (2592, 2300)
		// F - mid-half diamond in the spine. 2x, so it hides a standing body.
		{ 0.55f, 0.2917f, 1000.f, 1000.f, StructureHeight2x, 45.f },   // (5280, 1400)
		// G - the new one. A larger 2x diamond on the outer lane, halfway between F and C, which is
		//     the piece that keeps the second half of the midfield from being empty floor.
		{ 0.80f, 0.3542f, 1200.f, 1200.f, StructureHeight2x, 45.f }    // (7680, 1700)
	};

	/**
	 * Mirrored in X only: long 2x bars straddling the centreline of each half, so the shortest route
	 * from the centre diamond to a goal is never a clear straight line.
	 *
	 * ONE -> TWO with the lengthening. The spine is the route every carrier takes and it is now
	 * 16800 uu from the diamond to the goal line; a single bar in the middle of that leaves two
	 * clear 6000 uu runs either side of it.
	 */
	static constexpr float AxisCoverXFracs[] = { 0.2400f, 0.5600f };   // 4032, 9408
	static constexpr float AxisCoverSizeX = 2000.f;
	static constexpr float AxisCoverSizeY = 500.f;

	/**
	 * The 3.5x tower at TOP CENTRE, standing on the dividing line. Straight from the sketch.
	 *
	 * It is the one structure that is NOT mirrored, and it does not need to be: it sits on X = 0, so
	 * it belongs to neither half and the side switch cannot make it unfair. It is the tallest thing
	 * on the field outside the endzone gates, which makes it the landmark that tells you where the
	 * centre is from anywhere - the job the old 1800 uu dais pylons used to do badly.
	 *
	 * Checked: its Y span (2034..3166) clears the centre diamond's corner by 690 uu and the corner
	 * banks by 134 uu.
	 */
	static constexpr float TopCentreTowerYFrac = 0.5417f;   // 2600
	static constexpr float TopCentreTowerSide = 800.f;

	/**
	 * The mid-lane pylons: tall thin light columns marking the flank route, each carrying a light
	 * bridge out to the side wall.
	 *
	 * ONE PER QUADRANT -> TWO. They are the only vertical structure in the outer lane, and the outer
	 * lane is now 16800 uu long per half; a single column at 0.35 would have left 10000 uu of lane
	 * with nothing standing in it, which is the "empty flank" failure the whole flank section exists
	 * to answer. Two at 0.25 and 0.6667 put a column roughly every 5600 uu and give the light-bridge
	 * span a rhythm.
	 *
	 * Y stays at 0.6042 (2900) - the field width did not change - which keeps them 290 uu clear of
	 * the corner banks' toe.
	 */
	static constexpr float LanePylonXFracs[] = { 0.2500f, 0.6667f };   // 4200, 11200
	static constexpr float LanePylonYFrac = 0.6042f;   // 2900
	static constexpr float LanePylonSide = 220.f;
	static constexpr float LanePylonHeight = 1300.f;

	/**
	 * The four pylons standing around the centre diamond.
	 *
	 * Pulled in and shortened (2600/1600/1800 -> 2200/1100/1400) to match the smaller diamond: at
	 * the old radius they ringed a landmark that is no longer there, and at 1800 uu they competed
	 * with the top-centre tower for the eye. Checked against the diamond's base corner - the
	 * diamond is a 1900 uu square yawed 45, i.e. |x| + |y| <= 1344, and a pylon at (2200, 1100)
	 * sums to 3300.
	 */
	// 0.1833 -> 0.1310, i.e. the SAME 2200 uu on the longer field. These ring the centre diamond, and
	// the diamond did not get bigger when the field did; left as a fraction of the half length they
	// would have marched out to 3080 and stopped reading as its ring.
	static constexpr float DaisPylonXFrac = 0.1310f;   // 2200
	static constexpr float DaisPylonYFrac = 0.2292f;   // 1100
	static constexpr float DaisPylonSide = 240.f;
	static constexpr float DaisPylonHeight = 1400.f;

	/**
	 * Glowing ring on the floor marking the contested zone. Diameter as a fraction of HalfWidth.
	 *
	 * 0.8667 -> 0.62. It is a fraction of the half width, so it survived the narrowing arithmetically
	 * - and would have landed a 4160 uu radius ring straight through the toe of both corner banks,
	 * where the terraces would have eaten most of it. 0.62 gives a 2976 uu radius, which sits
	 * entirely on the flat playfield with 324 uu to spare.
	 */
	static constexpr float CentrePadDiameterFrac = 0.62f;
	static constexpr float CentreRingWidth = 70.f;

	// --- Flanks ----------------------------------------------------------------------------------
	//
	// THE PROBLEM THIS SOLVES. The interior layout above lives inside |Y| < 5900, but almost all of
	// its mass is inside |Y| < 4400, and the routes players actually take run down the spine. The
	// result, measured: the outer thirds of the field render as two black voids, and a screenshot
	// taken looking across one of them is half an empty frame. The perimeter walls DO carry trim, a
	// mid band and ribs - but at 6000 uu away, through fog, a 46 uu strip is a hairline.
	//
	// The fix is deliberately NOT more cover. More blocks in the wings would narrow the flanking
	// routes and hand the navmesh-less bots more to grind against. Instead everything here is either
	// flush against a wall or above head height:
	//
	//   buttresses ... 30 pilasters standing on the perimeter, giving the walls a rhythm and a
	//                  near-field object to judge distance and speed against
	//   high rail .... a continuous glowing line at 1640 uu carried along the tops of the tall
	//                  buttresses, which is what actually fills the upper half of a flank frame
	//   bridges ...... one beam per quadrant running from the top of a lane pylon out to the wall,
	//                  so the pylons read as part of a structure instead of as loose columns
	//   stripes ...... two bright floor lines per side, which the near-mirror floor doubles
	//   corner pylons  four tall columns in the corners behind the goal lines, the one part of the
	//                  field with no interior structure at all
	//
	// AFTER THE SECTION 7 REBUILD the void this was written for is much smaller - the field is 9600
	// wide rather than 12000, and the outer 1500 uu of each side is now a corner bank, which is
	// terrain the player can stand on rather than empty floor. All of it is kept because all of it
	// is either flush with a wall or above head height, so none of it fights the banks: the
	// buttresses and corner pylons are built from Z = 0 and simply emerge from the bank they stand
	// on, and the rail and bridges are at 1640 and 1240 uu, far above the 352 uu bank crest.

	struct FButtressSpec
	{
		float PositionFrac;   // of HalfLength for the side walls, of HalfWidth for the end walls
		float Height;
	};

	/**
	 * Side walls (constant Y). Mirrored into +/-X and +/-Y, so 0.f is built once per wall.
	 *
	 * FIVE -> SEVEN for the 3.5:1 field. The row's job is to give 33600 uu of wall a beat you can
	 * judge your own speed against, and a beat is a function of SPACING, not of count: at the old
	 * five fractions the gap between the centre buttress and its neighbour would have gone from 2400
	 * uu to 3360 and the next from 3600 to 5040. Seven lands them 2520 uu apart, which is the old
	 * spacing to within a hundred uu. Tall (1600) and short (1050) alternate so the high rail they
	 * carry has something to sit on every other bay.
	 */
	static const FButtressSpec SideButtresses[] =
	{
		{ 0.0000f, 1700.f },   // 0      - midfield marker, the tallest of the row
		{ 0.1500f, 1050.f },   // 2520
		{ 0.3000f, 1600.f },   // 5040
		{ 0.4500f, 1050.f },   // 7560
		{ 0.6000f, 1600.f },   // 10080
		{ 0.7300f, 1050.f },   // 12264
		{ 0.8600f, 1600.f }    // 14448  - just behind the goal line, inside the endzone
	};

	/** End walls (constant X). Mirrored into +/-Y and +/-X. Y = 0 is left clear for the scoring lane. */
	static const FButtressSpec EndButtresses[] =
	{
		{ 0.3000f, 1100.f },   // 1800
		{ 0.5667f, 1600.f },   // 3400
		{ 0.8333f, 1100.f }    // 5000
	};

	static constexpr float ButtressWidth = 420.f;
	static constexpr float ButtressDepth = 200.f;

	/**
	 * Height of the rail running along the buttress row.
	 *
	 * Sits 40 uu above the 1600 uu buttresses so it reads as something they CARRY. It is unlit neon
	 * with no collision, which matters: at 1640 uu it is well above any jump (JumpZVelocity 420
	 * gives a ~90 uu apex) so nobody can ever touch it, and a blocking box up there would only ever
	 * be an invisible obstacle for hitscan.
	 */
	static constexpr float FlankRailZ = 1640.f;
	static constexpr float FlankRailSize = 64.f;

	/** Light bridge: lane pylon top out to the side wall. Cross-section, and the drop below the top. */
	static constexpr float BridgeSize = 78.f;
	static constexpr float BridgeDrop = 60.f;

	/**
	 * Lane floor stripes, as fractions of HalfWidth.
	 *
	 * { 0.55, 0.8833 } -> { 0.42, 0.66 }, i.e. 2016 and 3168 rather than 3300 and 5300. These are
	 * floor decals 7 uu off the deck: at the old fractions the outer one would have been at Y = 4240
	 * and the inner one exactly ON the bank toe, so one stripe would have been entirely buried
	 * inside a terrace and the other half-eaten. Both now run on the flat playfield, and they mark
	 * the two routes that actually exist there - the spine and the lane between the spine and the
	 * bank.
	 */
	static constexpr float LaneStripeYFracs[] = { 0.4200f, 0.6600f };   // 2016, 3168
	static constexpr float LaneStripeWidth = 72.f;

	/**
	 * Corner pylons, in the dead space behind each goal line.
	 *
	 * NO LONGER A FRACTION OF THE HALF LENGTH. They belong to the ENDZONE, whose depth is an absolute
	 * 2400 uu, so a length fraction slides them relative to the thing they are dressing: 0.8833 of
	 * the new half length is X = 14840, which is 440 uu behind the goal line and 80 uu from the face
	 * of a gate tower - two structures visibly fouling each other, from a change of field length.
	 * They now stand at the MIDDLE OF THE ENDZONE (goal line + half the depth, X = 15600), which is
	 * where they were in spirit on the old field and where they stay whatever the field does. Y is
	 * still a width fraction, because the width is what it is dressing.
	 */
	static constexpr float CornerPylonYFrac = 0.8500f;   // 4080
	static constexpr float CornerPylonSide = 300.f;
	static constexpr float CornerPylonHeight = 1900.f;

	// --- Floor lamps -----------------------------------------------------------------------------
	//
	// A symmetric lattice of unshadowed point lights at knee-to-waist height. Mirrored into all four
	// quadrants, so the array below is one quadrant's worth: 6 x 4 = 24 lamps in total.
	//
	// These are the only lights in the rig with a LOW incidence angle. KeyLight, FillLight and
	// BounceLight are all directional and all steep, which is fine for the floor and for character
	// tops and useless for the vertical face of a cover block. A lamp sitting on the deck 2000 uu
	// away rakes across those faces and is what finally makes a slab read as a solid object rather
	// than as a hole in the world.
	//
	// THREE COLUMNS -> FOUR for the 3.5:1 field, and this one is not decoration. The lattice spacing
	// has to stay near FloorLampRadius (4200) or neighbouring pools stop touching and the field goes
	// back to being lit only by the three high directional lights - which is the exact condition that
	// made a 900 uu cover block render as a featureless black shape. At the old three fractions the
	// columns would have landed 5880 uu apart on the longer field, i.e. 1680 uu of gap between the
	// edges of adjacent pools, in the two places (X ~ 4900 and ~ 12600) the new cover scatter lives.
	// Four fractions land them 4032 uu apart, just inside the radius. Cost: 24 -> 32 unshadowed
	// point lights.
	static constexpr float LampXFracs[] = { 0.1400f, 0.3800f, 0.6200f, 0.8600f };   // 2352, 6384, 10416, 14448
	static constexpr float LampYFracs[] = { 0.3600f, 0.8000f };                     // 1728, 3840

	/**
	 * Lamp height. MEASURED: this started at 500 and the floor directly beneath a lamp came back as a
	 * blown white-hot blob - inverse-square falloff means 220 cd at 5 m is 8.8 lux, three times the
	 * key light, on a floor whose whole job is to be near black. Lifting the lamps to 9 m drops that
	 * peak to 2.7 lux (about the key) and flattens the gradient across the floor, while costing
	 * almost nothing at the ranges that matter, because at 20 m the extra 4 m of height is lost in
	 * the hypotenuse. Height is the cheap dial here; intensity is the expensive one.
	 */
	static constexpr float LampZ = 900.f;

	/**
	 * Endzone gate: two towers on the goal line plus a beam spanning the WHOLE width above them.
	 *
	 * The towers used to stand at |Y| = 3000, i.e. at half width, and that was the single worst piece
	 * of misinformation in the arena. The scoring volume has always covered the full field width,
	 * but the gate is what the eye reads as "the endzone starts here", so the arena said the zone was
	 * the middle half of the field while the trigger said sideline to sideline. Two towers on the
	 * sidelines with a beam right across say the true thing, and they clear the scoring lane in the
	 * middle of the goal line, which is where the carrier is actually running.
	 *
	 * GateTowerWallGap is measured from the side wall's inner face to the tower's OUTER face. 300
	 * clears the buttress row (ButtressDepth 200) and the flank rail hanging off it (FlankRailSize 64
	 * immediately inboard of that) with 36 uu to spare, so the gate never intersects the flank
	 * dressing. Re-check it if you change either of those.
	 */
	static constexpr float GateTowerWallGap = 300.f;
	static constexpr float GateTowerSide = 420.f;
	static constexpr float GateTowerHeight = 2300.f;
	static constexpr float GateBeamSize = 300.f;

	// --- MODE B: the goal (spec v4 section 7) ------------------------------------------------------
	//
	// Verbatim: "scoring should happen when the core is thrown into the goal or a player carries the
	// core into the goal. The goal should not be the entire width of the map, like the endzone."
	//
	// THE SHAPE. A goal is the middle GoalWidthFieldFraction of the field width (0.2083 of a 9600 uu
	// field, so a 2000 uu mouth) from the goal line back to the end wall, and GoalHeightUU (440) tall.
	// Both shrank in spec v5 section 4 - "for game mode b ONLY ... decrease the size of the goal
	// (reduce height and width)" - and both shrank by the SAME factor, so the mouth keeps its
	// proportions. Mode A never reads either number: its endzones span the full width and wall height. It
	// is drawn as an actual goal: two posts standing ON the goal line at the edges of the mouth, a
	// crossbar between them at the top of the volume, a lit sill across the mouth on the floor and a
	// lit patch of floor inside it. That set of five lines is what makes the volume READ - a player
	// has to be able to see, from midfield, both where the mouth is and how high it goes, because in
	// mode B the throw is aimed at it.
	//
	// WHY THE POSTS BLOCK. They are the only piece of mode-B furniture with collision, and it is
	// deliberate: a goal you can throw THROUGH the frame of is not a goal, and a post you can bounce
	// a thrown Core off is the counterplay that makes aiming matter. They are thin (140 uu) and stand
	// 1600 uu off the centre line, so they are nowhere near the running lanes a bot uses to reach the
	// endzone - and they are pawn-blocking only in the sense every other pylon in the arena is.
	//
	// The DEPTH is the endzone depth, deliberately not its own dial: the goal occupies the middle
	// third of the endzone footprint, so a Core thrown to the back of the net still counts and the
	// two shapes cannot end up describing different rooms.
	static constexpr float GoalPostSide = 140.f;
	static constexpr float GoalPostOvershoot = 90.f;    // posts stand this far above the crossbar
	static constexpr float GoalCrossbarSize = 120.f;

	/** Floor sill across the goal mouth, and the lit patch behind it. */
	static constexpr float GoalSillWidth = 90.f;
	static constexpr float GoalPatchZ = 5.f;            // above PatchZ (4) so it wins the z-fight

	/** Glow for the goal frame. Brighter than a gate tower: it is the thing being aimed at. */
	static constexpr float GlowGoalFrame = 5.2f;

	/**
	 * Clamps on the two mode-B settings, so a live edit in PIE can never build a degenerate goal.
	 * The upper width bound is 0.9 rather than 1.0 because a full-width "goal" is an endzone, and
	 * silently rebuilding mode A inside mode B is worse than refusing.
	 */
	static constexpr float MinGoalWidthFraction = 0.05f;
	static constexpr float MaxGoalWidthFraction = 0.90f;
	static constexpr float MinGoalHeight = 120.f;

	// --- MODE B, SPEC v6 section 4.3: the ring in the back wall -------------------------------------
	//
	// The goal is no longer furniture standing in front of a wall - it is a HOLE THROUGH the wall
	// with a lit hoop around it. Which means the end wall itself is now mode-tagged: the solid slab
	// BuildFloorAndWalls makes belongs to mode A, and BuildGoalWall makes a perforated replacement
	// that belongs to mode B. Exactly one of them is presented at a time, so mode A's end wall is the
	// same single block it has always been and cannot acquire a hole.
	//
	// HOW THE HOLE IS MADE, since "subtract a cylinder from a box" is not something a box collider
	// can do. Four panels leave a SQUARE opening of half side RingSquareScale x radius, and a thick
	// annulus of RingSegments spokes closes that square down to a circle. Each spoke is a box whose
	// inner face is a chord at exactly the ring radius, so the union of their inner faces is a
	// regular polygon CIRCUMSCRIBING the scoring disc - the physical hole is never smaller than the
	// disc that scores, which is the direction the error has to lean: a hole a hair too small would
	// bounce Cores off a goal the rule says went in.
	//
	// The square must be wide enough to clear the disc and narrow enough that its corners stay inside
	// the annulus: radius <= S <= outer/sqrt(2). At 1.05 and 1.55 there is room at both ends.
	static constexpr int32 GoalRingSegments = 16;
	static constexpr float GoalRingOuterScale = 1.55f;   // annulus outer radius / mouth radius
	static constexpr float GoalRingSquareScale = 1.05f;  // half side of the square opening / radius
	static constexpr float GoalRingSpokeOverlap = 1.12f; // tangential slop so adjacent spokes meet

	/** Radial depth and forward proudness of the glowing hoop laid on the annulus's inner rim. */
	static constexpr float GoalRingRimDepth = 64.f;
	static constexpr float GoalRingRimProud = 40.f;

	/**
	 * Glow for the hoop. The brightest thing in the arena on purpose: spec v6 asks to "make it
	 * visually obvious what and where the goal is", the ring is set into a near-black wall 16800 uu
	 * from the far spawn, and it is the thing every throw in mode B is aimed at.
	 */
	static constexpr float GlowGoalRing = 7.5f;

	/** Depth of the lit alcove behind the ring, so the hoop frames a room and not the empty sky. */
	static constexpr float GoalAlcoveDepth = 700.f;

	/** Thickness of the approach ramp slab. */
	static constexpr float GoalRampThickness = 140.f;

	/** Headroom left between the top of the hoop and the top of the wall. */
	static constexpr float GoalRingWallHeadroom = 120.f;
}

ATraceArenaBuilder::ATraceArenaBuilder()
{
	PrimaryActorTick.bCanEverTick = false;

	// Replicated so clients receive the actor and run BeginPlay locally; it owns no replicated
	// properties, and its transform is sent once in the spawn header. Movement replication is off
	// because the arena never moves - and if it did, every client's copy of the geometry would
	// still be sitting where it was built.
	bReplicates = true;
	SetReplicateMovement(false);
	SetCanBeDamaged(false);

	// MEASURED BUG, FIXED (spec v5 section 0). The comment that used to live here argued that
	// relevancy did not matter "because the geometry is local and already built". That is true for
	// going irrelevant MID-MATCH and false for the only case that counts: the FIRST replication.
	//
	// The builder is spawned at the origin, server-only, by ATraceGameMode. A client never runs
	// BeginPlay - and so never calls EnsureBuilt() - until the actor arrives over the wire. With
	// the default NetCullDistanceSquared of 225000000 (15000 uu) and player starts at X = +-15600,
	// the actor is out of range from the very first frame and NEVER arrives. The client log
	// contained no "Arena built" line at all and the joining pawn fell to Z = -19636.
	//
	// bAlwaysRelevant is still a plain public uint8:1 on AActor in 5.8 - ATraceCore sets it the
	// same way a few files over, so the accessor-form worry in the old comment is settled. It costs
	// one relevancy result per connection for an actor with zero replicated properties.
	bAlwaysRelevant = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	// Explicitly Movable: a scene component may never be less mobile than its children, and every
	// piece of geometry we attach below is created Movable (runtime-spawned actors have no baked
	// lighting to gain from Static, and this project disables static lighting outright).
	Root->SetMobility(EComponentMobility::Movable);

	// Constructor-time FObjectFinders are what make these engine assets cook into a packaged build.
	// A bare runtime LoadObject would resolve to null once cooked (build contract section 2).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		CubeMesh = CubeFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		CylinderMesh = CylinderFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		BaseMaterial = MaterialFinder.Object;
	}

	// The two generated Tron materials. These are NOT in the repo (Content/Generated/ is gitignored)
	// and are produced by Scripts/generate_content.py - see the asset note in the class comment. A
	// missing asset here is a soft failure: MakeSurfaceMID/MakeNeonMID fall back to BasicShapeMaterial
	// and the arena renders flat but still plays. FObjectFinder logs the miss, which is exactly the
	// breadcrumb a developer who has not run the script needs.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SurfaceFinder(TEXT("/Game/Generated/Materials/M_TraceSurface.M_TraceSurface"));
	if (SurfaceFinder.Succeeded())
	{
		SurfaceMaterial = SurfaceFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		NeonMaterial = NeonFinder.Object;
	}
}

// -------------------------------------------------------------------------------------------------
// Contract surface
// -------------------------------------------------------------------------------------------------

ATraceArenaBuilder* ATraceArenaBuilder::Get(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<ATraceArenaBuilder> It(World); It; ++It)
	{
		ATraceArenaBuilder* Builder = *It;
		if (IsValid(Builder))
		{
			return Builder;
		}
	}

	return nullptr;
}

float ATraceArenaBuilder::DaisTopZ() const
{
	return bBuildInteriorLayout ? (TraceArenaConstants::StepRise * TraceArenaConstants::DaisTiers) : 0.f;
}

float ATraceArenaBuilder::PlayerHeightUU() const
{
	// Read the capsule, do not assume it. Spec v3 section 7 sizes every structure in the arena as a
	// multiple of ONE PLAYER HEIGHT, and the only durable definition of that is the pawn the player
	// actually drives: ATraceCharacter sets its capsule to 88 uu half height in its constructor, so
	// the class default object already knows the answer before any pawn exists. Cached statically
	// because the CDO cannot change within a process and this is called once per structure.
	static const float Cached = []() -> float
	{
		const ATraceCharacter* CDO = ATraceCharacter::StaticClass()->GetDefaultObject<ATraceCharacter>();
		const UCapsuleComponent* Capsule = (CDO != nullptr) ? CDO->GetCapsuleComponent() : nullptr;
		if (Capsule == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("ATraceArenaBuilder: could not read the character capsule; structure heights fall back to %.0f uu per player height."),
				TraceArenaConstants::FallbackPlayerHeight);
			return TraceArenaConstants::FallbackPlayerHeight;
		}

		const float Height = Capsule->GetUnscaledCapsuleHalfHeight() * 2.f;

		// Logged at Log, not Verbose. "Why is that block the wrong height" is otherwise a question
		// you can only answer by reading two files, and a previous pass twice declared a working
		// mechanic dead because its log line was suppressed by default.
		UE_LOG(LogTraceGame, Log,
			TEXT("Arena structure heights keyed to the capsule: 1 player height = %.0f uu, so 1x/2x/3.5x = %.0f/%.0f/%.0f."),
			Height, Height, Height * 2.f, Height * 3.5f);

		return FMath::Max(1.f, Height);
	}();

	return Cached;
}

float ATraceArenaBuilder::BankInnerHalfWidth() const
{
	if (!bBuildCornerBanks)
	{
		return HalfWidth();
	}

	// Never let a silly BankDepth swallow the whole field: at minimum a quarter of the half width
	// stays flat, which is what everything laid on the floor is placed against.
	const float Depth = FMath::Clamp(BankDepth, 0.f, HalfWidth() * 0.75f);
	return HalfWidth() - Depth;
}

FVector ATraceArenaBuilder::GetCoreSpawnLocation() const
{
	// Just above the pedestal top, which now stands on the dais rather than on the floor. The Core is
	// spawned/reset here and drops the last few uu onto it. ATraceCore treats anything within 75 uu
	// of this point as "already home", so the resting position must stay inside that tolerance -
	// keep CoreDropHeight small.
	const FVector Local(0.f, 0.f, DaisTopZ() + TraceArenaConstants::PedestalHeight + TraceArenaConstants::CoreDropHeight);
	return GetActorTransform().TransformPosition(Local);
}

FBox ATraceArenaBuilder::GetFieldBounds() const
{
	const FBox Local(
		FVector(-HalfLength(), -HalfWidth(), 0.f),
		FVector(HalfLength(), HalfWidth(), WallHeight));

	return Local.TransformBy(GetActorTransform());
}

float ATraceArenaBuilder::ClampedEndzoneDepth() const
{
	// Upper bound is the HALF length, not the length: a zone deeper than that at each end would meet
	// in the middle, and the derived spawn line would land on the centre dais - which is exactly the
	// failure a previous pass shipped when this clamp existed in three copies and one of them used a
	// Max(0, ...) instead. One function, one answer.
	return FMath::Clamp(EndzoneDepth, 100.f, HalfLength());
}

float ATraceArenaBuilder::GoalLineX() const
{
	// |X| of a goal line. ClampedEndzoneDepth(), never the raw property - see that function.
	return HalfLength() - ClampedEndzoneDepth();
}

FBox ATraceArenaBuilder::GetEndzoneBounds(float EndSign) const
{
	const float Sign = (EndSign < 0.f) ? -1.f : 1.f;
	const float HalfX = HalfLength();

	// Goal line to end wall along X; sideline to sideline along Y; floor to wall top in Z. The Y
	// term is the full half width on purpose - see EndzoneHalfWidth().
	const float NearX = Sign * GoalLineX();
	const float FarX = Sign * HalfX;

	const FBox Local(
		FVector(FMath::Min(NearX, FarX), -EndzoneHalfWidth(), 0.f),
		FVector(FMath::Max(NearX, FarX), EndzoneHalfWidth(), WallHeight));

	return Local.TransformBy(GetActorTransform());
}

float ATraceArenaBuilder::GoalHalfWidth() const
{
	// The FRACTION IS OF THE FULL WIDTH, so halving it twice is correct and is the mistake to watch
	// for: at the v5 default this is 2000 uu of mouth, i.e. 1000 either side of the centre line.
	const float Fraction = FMath::Clamp(UTraceSettings::Get().GoalWidthFieldFraction,
		TraceArenaConstants::MinGoalWidthFraction, TraceArenaConstants::MaxGoalWidthFraction);

	// Never wider than the field itself, whatever the settings say.
	return FMath::Min(HalfWidth(), FieldWidth * Fraction * 0.5f);
}

float ATraceArenaBuilder::ClampedGoalHeight() const
{
	return FMath::Clamp(UTraceSettings::Get().GoalHeightUU, TraceArenaConstants::MinGoalHeight, WallHeight);
}

float ATraceArenaBuilder::GoalRingClearanceZ() const
{
	// "Raise the goals 1.5x player height from the ground" (spec v6 section 4.3), measured to the
	// BOTTOM of the hoop - see the long note in the header for why the centre reading is the one that
	// had to give. Keyed to the capsule, like every other height in this arena, so shrinking the pawn
	// lowers the goal with it.
	const float Raw = FMath::Max(0.f, GoalRingRaisePlayerHeights) * PlayerHeightUU();

	// Never above the middle of the wall: a hoop that starts higher than that cannot also be 2000 uu
	// across, and silently shrinking the mouth to fit would break the other half of the spec.
	return FMath::Clamp(Raw, 0.f, FMath::Max(0.f, WallHeight * 0.5f - 100.f));
}

float ATraceArenaBuilder::GoalRingRadius() const
{
	// The MOUTH HALF WIDTH is the ring's radius, so spec v5's 2000 uu mouth becomes spec v6's 2000 uu
	// diameter and UTraceSettings::GoalWidthFieldFraction goes on meaning exactly what it says.
	// Clamped so the whole hoop fits between its own floor clearance and the top of the wall.
	const float Room = (WallHeight - GoalRingClearanceZ() - TraceArenaConstants::GoalRingWallHeadroom) * 0.5f;
	return FMath::Clamp(GoalHalfWidth(), 120.f, FMath::Max(120.f, FMath::Min(Room, HalfWidth() * 0.9f)));
}

float ATraceArenaBuilder::GoalRingCentreZ() const
{
	return GoalRingClearanceZ() + GoalRingRadius();
}

float ATraceArenaBuilder::GoalRampTopZ() const
{
	if (GoalRampRunPerRise <= 0.f)
	{
		return 0.f;
	}

	// ONE STEP BELOW THE HOOP, and that single step is the whole design of this ramp. Higher and the
	// ramp would occlude the bottom of the mouth (and a low throw that should have been a turnover
	// would land on the ramp INSIDE the goal); lower and a carrier standing on it is not inside the
	// disc. At the default it is 224 uu, which puts a standing carrier's origin at 312 uu - 952 uu
	// from the ring centre, i.e. inside a 1000 uu mouth with 48 uu to spare.
	//
	// UTraceSettings::GoalHeightUU IS THE DIAL, and this is the whole of what spec v6 left it doing.
	// Before v6 it was the height of a goal that stood on the floor; there is no such goal any more,
	// and a slider that moves nothing is worse than no slider - this project's rule, and it is why
	// this reads it rather than leaving it stranded. At its shipped 440 the clamp below wins and the
	// ramp is exactly where the ring puts it; lowering it lowers the run-up, which is a real and
	// legible thing to tune (how easy is it to carry one in), and 0 removes the ramp entirely.
	return FMath::Clamp(ClampedGoalHeight(), 0.f,
		FMath::Max(0.f, GoalRingClearanceZ() - TraceArenaConstants::StepRise));
}

FVector ATraceArenaBuilder::GetGoalRingCentre(float EndSign) const
{
	const float Sign = (EndSign < 0.f) ? -1.f : 1.f;
	return GetActorTransform().TransformPosition(FVector(Sign * HalfLength(), 0.f, GoalRingCentreZ()));
}

FBox ATraceArenaBuilder::GetGoalBounds(float EndSign) const
{
	const float Sign = (EndSign < 0.f) ? -1.f : 1.f;
	const float HalfX = HalfLength();
	const float Radius = GoalRingRadius();
	const float CentreZ = GoalRingCentreZ();
	const float Depth = FMath::Clamp(GoalRingDepth, 60.f, ClampedEndzoneDepth());

	// SPEC v6 section 4.3. The goal is no longer the endzone-deep box it was in v4/v5: it is a shallow
	// SLAB across the mouth of the ring in the back wall, and the disc inscribed in that slab is what
	// actually scores (ATraceEndzone::ConfigureRing). This box is the slab, i.e. the ring's bounding
	// volume - it is what bot targeting and debug draw read, and it is deliberately conservative, so
	// anything that treats it as the goal aims at the middle of the hoop rather than past it.
	const float NearX = Sign * (HalfX - Depth);
	const float FarX = Sign * HalfX;

	const FBox Local(
		FVector(FMath::Min(NearX, FarX), -Radius, CentreZ - Radius),
		FVector(FMath::Max(NearX, FarX), Radius, CentreZ + Radius));

	return Local.TransformBy(GetActorTransform());
}

FBox ATraceArenaBuilder::GetScoringBounds(float EndSign) const
{
	return TraceIsGoalMode(ScoringMode) ? GetGoalBounds(EndSign) : GetEndzoneBounds(EndSign);
}

// -------------------------------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------------------------------

void ATraceArenaBuilder::BeginPlay()
{
	Super::BeginPlay();

	// Normally already built: ATraceGameMode calls EnsureBuilt() from PreInitializeComponents, long
	// before anything begins play. This covers a level-placed builder in a map with no Trace game
	// mode, and clients (who have no game mode at all and build their copy off the replicated actor).
	EnsureBuilt();
}

void ATraceArenaBuilder::EnsureBuilt()
{
	if (!bArenaBuilt)
	{
		BuildArena();
	}
}

void ATraceArenaBuilder::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Only clean up when this actor specifically is destroyed. On level teardown or travel the
	// world destroys everything anyway, and touching actors mid-teardown is a good way to crash.
	if (EndPlayReason == EEndPlayReason::Destroyed)
	{
		for (const TObjectPtr<AActor>& Spawned : SpawnedActors)
		{
			AActor* Actor = Spawned.Get();
			if (IsValid(Actor))
			{
				Actor->Destroy();
			}
		}
	}

	SpawnedActors.Reset();

	Super::EndPlay(EndPlayReason);
}

// -------------------------------------------------------------------------------------------------
// Build
// -------------------------------------------------------------------------------------------------

void ATraceArenaBuilder::BuildArena()
{
#if WITH_EDITOR
	// Belt and braces against the one way an editor preview could ever hurt a running game: if a
	// preview is somehow still standing on this actor when the real build starts, tear it down first
	// so the arena cannot exist twice. Nothing should be able to reach here with a preview up - the
	// preview only builds in an editor world and everything it makes is transient - but "two floors,
	// two sets of endzone triggers" is a bad enough failure to be worth one branch.
	if (bEditorPreviewBuilt)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("ATraceArenaBuilder: an editor preview was still standing at build time; clearing it first."));
		DestroyBuiltArena();
	}
#endif

	bArenaBuilt = true;

	// WHICH SCORING SHAPE TO PRESENT, decided once here and re-applied whenever the authority says it
	// changed (ApplyScoringModeInWorld). ATraceGameState is the authority and it is asked first; on a
	// machine that has no game state yet - the editor preview, or a build that runs before the game
	// mode has published - the settings page is the best available guess, and it is only a guess, so
	// the later ApplyScoringMode call is what makes it right. Both shapes are built either way, so a
	// wrong guess here costs a flag flip and nothing else.
	{
		const ETraceScoringMode PublishedMode = ATraceGameState::GetScoringModeFor(this);
		ScoringMode = (GetWorld() != nullptr && GetWorld()->GetGameState() != nullptr)
			? PublishedMode
			: UTraceSettings::Get().ScoringMode;
	}

	// A dedicated server needs collision and triggers, nothing else: material shaders are not cooked
	// for server targets and nothing there ever renders.
	const bool bBuildVisuals = (GetNetMode() != NM_DedicatedServer);

	if (bBuildVisuals && (SurfaceMaterial == nullptr || NeonMaterial == nullptr))
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("ATraceArenaBuilder: /Game/Generated/Materials is missing, falling back to BasicShapeMaterial. ")
			TEXT("The arena will render flat and lit instead of neon. Run Scripts/generate_content.py."));
	}

	BuildFloorAndWalls(bBuildVisuals);

	if (bBuildVisuals)
	{
		BuildGrid();
	}

	// The banks are the ground, so they go down before anything that stands on them. Gated on their
	// own switch rather than on bBuildInteriorLayout: a flat field with cover and a bowl with no
	// cover are both useful things to be able to look at while tuning.
	if (bBuildCornerBanks)
	{
		BuildCornerBanks(bBuildVisuals);
	}

	if (bBuildInteriorLayout)
	{
		BuildCentreDais(bBuildVisuals);
		BuildCoverField(bBuildVisuals);
	}

	if (bBuildFlankStructures)
	{
		BuildFlanks(bBuildVisuals);
	}

	// BOTH scoring shapes, always. See the two-modes note in the header: the A/B toggle has to be
	// survivable mid-match, and the only way to do that safely is to have already built what the
	// other mode needs.
	BuildEndzones(bBuildVisuals);
	BuildGoals(bBuildVisuals);

	if (HasAuthority())
	{
		// Only ChoosePlayerStart reads these, and that only ever runs on the server.
		BuildPlayerStarts();
	}

	BuildLighting();

	if (bBuildVisuals)
	{
		BuildFloorLamps();
	}

	BuildPostProcess();

	// Present one of the two scoring shapes and disarm the other. Forced rather than early-returned
	// on "the mode did not change", because at this point NOTHING has been presented yet: the goal
	// furniture is built visible and the endzone triggers are built armed, so mode A needs this call
	// exactly as much as mode B does.
	ApplyScoringMode(ScoringMode);

	// Spelled out at Log because the proportion, the flat playfield width and which game is being
	// played are the things a playtester asks about first, and none of them is readable from a
	// screenshot.
	UE_LOG(LogTraceGame, Log,
		TEXT("Arena built (%.0f x %.0f x %.0f uu, %.2f:1, flat playfield %.0f wide, banks %s at %.0f uu, ")
		TEXT("mode %s, visuals=%s, authority=%s): %d components."),
		FieldLength, FieldWidth, WallHeight,
		(FieldWidth > 0.f) ? (FieldLength / FieldWidth) : 0.f,
		BankInnerHalfWidth() * 2.f,
		bBuildCornerBanks ? TEXT("on") : TEXT("off"), BankHeight,
		*TraceScoringModeLabel(ScoringMode),
		bBuildVisuals ? TEXT("yes") : TEXT("no"),
		HasAuthority() ? TEXT("yes") : TEXT("no"),
		GetComponents().Num());
}

void ATraceArenaBuilder::BuildFloorAndWalls(bool bBuildVisuals)
{
	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();

	// The slab reaches under the walls so nothing can squeeze through the seam at the corners.
	const FVector FloorSize(FieldLength + 2.f * WallThickness, FieldWidth + 2.f * WallThickness, FloorThickness);
	const FVector FloorCenter(0.f, 0.f, -FloorThickness * 0.5f);

	AddCollisionBlock(FloorCenter, FloorSize, TEXT("FloorCollision"));

	if (bBuildVisuals)
	{
		// Roughness 0.16: the floor is a near-black mirror. Almost everything you see on the ground is
		// the screen-space reflection of the neon above it, not the albedo - that reflected glow is
		// what a Tron floor actually is. Raise the roughness and the whole arena goes matte and dead.
		UMaterialInstanceDynamic* FloorMID = MakeSurfaceMID(TraceArenaConstants::FloorColor, 0.16f, 0.f);
		AddMeshBlock(CubeMesh, FloorCenter, FloorSize, FloorMID, /*bCastShadow=*/false, TEXT("Floor"));
	}

	// Four perimeter walls. The long pair overlaps the ends by a wall thickness so the corners are
	// solid without a fifth piece.
	const float WallCenterZ = WallHeight * 0.5f;

	struct FWallSpec
	{
		FVector Center;
		FVector Size;
		const TCHAR* Name;
	};

	const FWallSpec Walls[] =
	{
		{ FVector(0.f,  HalfY + WallThickness * 0.5f, WallCenterZ), FVector(FieldLength + 2.f * WallThickness, WallThickness, WallHeight), TEXT("WallPosY") },
		{ FVector(0.f, -HalfY - WallThickness * 0.5f, WallCenterZ), FVector(FieldLength + 2.f * WallThickness, WallThickness, WallHeight), TEXT("WallNegY") },
		{ FVector( HalfX + WallThickness * 0.5f, 0.f, WallCenterZ), FVector(WallThickness, FieldWidth, WallHeight), TEXT("WallPosX") },
		{ FVector(-HalfX - WallThickness * 0.5f, 0.f, WallCenterZ), FVector(WallThickness, FieldWidth, WallHeight), TEXT("WallNegX") }
	};

	// The same faint self-lit tint the interior structure carries, and for the same reason: at
	// point-blank range no trim of any kind is inside the field of view, and emissive is the only
	// term that does not depend on an angle of incidence. It is the difference between "I am against
	// a wall" and "my screen went black".
	UMaterialInstanceDynamic* WallMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::WallColor, 0.55f, 0.f, TraceArenaConstants::NeonNeutral, 0.022f)
		: nullptr;

	for (const FWallSpec& Wall : Walls)
	{
		// THE TWO END WALLS ARE MODE-TAGGED FROM SPEC v6 SECTION 4.3 ONWARDS, and the side walls are
		// not. Mode B puts a 2000 uu hole through each end wall for the ring goal, which cannot be
		// done by subtracting from a box - so mode B builds its own perforated replacement
		// (BuildGoalWall) and this solid slab is presented only while mode A is armed.
		//
		// Marked around the pair of components rather than around the loop, so the collision box and
		// the mesh for one wall are tagged together and the SIDE walls stay untagged and permanent.
		const bool bEndWall = (Wall.Size.X <= WallThickness * 1.01f);
		const int32 WallMark = bEndWall ? MarkBuiltComponents() : 0;

		AddCollisionBlock(Wall.Center, Wall.Size, Wall.Name);

		if (bBuildVisuals)
		{
			AddMeshBlock(CubeMesh, Wall.Center, Wall.Size, WallMID, /*bCastShadow=*/true, Wall.Name);
		}

		if (bEndWall)
		{
			CollectPiecesSince(WallMark, EndzoneModePieces);
		}
	}

	// --- Wall standoff ---------------------------------------------------------------------------
	//
	// THE MEASURED WORST CASE OF THE POINT-BLANK DEFECT LIVED HERE. A wall rib was 36 uu deep and
	// placed at HalfY - WallRibDepth*0.5, i.e. it occupied the 36 uu of PLAYABLE SPACE immediately
	// in front of a wall whose collision face is the wall itself. With a 34 uu capsule radius the
	// eye could not merely approach that slab, it ended up 2 uu INSIDE it - 90 uu of unlit emissive
	// filling the entire frame, at full intensity, because unlit emissive does not attenuate. That
	// is the 99%-blown-out frame in the defect report.
	//
	// A pawn-only shell WallNeonStandoff deep runs the length of each wall's inner face. Combined
	// with the narrowed, shallower ribs above it puts the eye 74 uu off the wall and 62 uu off the
	// rib. Pawn-only matters as much here as anywhere: shots, sightlines, the Core and the camera
	// probe all still see the wall exactly where it has always been, so nothing about shooting along
	// a wall or hugging it with the third-person camera changes. Built outside the bBuildVisuals
	// gate because a dedicated server must build the collision its clients are predicting against.
	const float StandoffZ = WallHeight * 0.5f;
	for (const float YSign : { 1.f, -1.f })
	{
		AddPawnStandoff(
			FVector(0.f, YSign * (HalfY - TraceArenaConstants::WallNeonStandoff * 0.5f), StandoffZ),
			FVector(FieldLength + 2.f * WallThickness, TraceArenaConstants::WallNeonStandoff, WallHeight),
			TEXT("WallStandoffY"));
	}
	for (const float XSign : { 1.f, -1.f })
	{
		// Mode-tagged with the end wall it belongs to. A full-face pawn shell across an end wall would
		// hold a mode-B carrier 74 uu OFF the goal they are trying to run through - an invisible wall
		// standing in the mouth, which is exactly the failure SetPiecesPresented was written to stop.
		// BuildGoalWall builds the four shells that go around the ring instead.
		const int32 StandoffMark = MarkBuiltComponents();

		AddPawnStandoff(
			FVector(XSign * (HalfX - TraceArenaConstants::WallNeonStandoff * 0.5f), 0.f, StandoffZ),
			FVector(TraceArenaConstants::WallNeonStandoff, FieldWidth, WallHeight),
			TEXT("WallStandoffX"));

		CollectPiecesSince(StandoffMark, EndzoneModePieces);
	}

	if (!bBuildVisuals)
	{
		return;
	}

	// --- Wall trim -------------------------------------------------------------------------------
	//
	// 2600 uu of unbroken dark wall is a void with a line on top. Three passes fix that: a bright
	// strip along the very top edge, a dimmer horizontal band at 42% height, and vertical ribs every
	// 3000 uu. Together they give the walls a scale and a rhythm you can judge distance against,
	// which on a field this large matters more than it did at 8000 uu.
	//
	// The end walls take the colour of the team that DEFENDS that end, so you can read which way you
	// are attacking from anywhere on the field. The side walls stay neutral cyan.

	const float TrimZ = WallHeight - TraceArenaConstants::WallTrimSize * 0.5f;
	const float TrimInset = TraceArenaConstants::WallTrimSize * 0.5f;
	const float BandZ = WallHeight * TraceArenaConstants::WallBandHeightFraction;
	const float KickZ = WallHeight * TraceArenaConstants::WallKickBandHeightFraction;
	const float RibHeight = WallHeight - TraceArenaConstants::WallTrimSize;
	const float RibZ = RibHeight * 0.5f;

	UMaterialInstanceDynamic* NeutralTrimMID = MakeNeonMID(TraceArenaConstants::NeonNeutral, TraceArenaConstants::GlowTrim);
	UMaterialInstanceDynamic* NeutralRibMID = MakeNeonMID(TraceArenaConstants::NeonNeutral, TraceArenaConstants::GlowRib);

	for (const float YSign : { 1.f, -1.f })
	{
		AddMeshBlock(CubeMesh, FVector(0.f, YSign * (HalfY - TrimInset), TrimZ),
			FVector(FieldLength, TraceArenaConstants::WallTrimSize, TraceArenaConstants::WallTrimSize),
			NeutralTrimMID, false, TEXT("WallTrimY"));

		AddMeshBlock(CubeMesh, FVector(0.f, YSign * (HalfY - TraceArenaConstants::WallBandSize * 0.5f), BandZ),
			FVector(FieldLength, TraceArenaConstants::WallBandSize, TraceArenaConstants::WallBandSize),
			NeutralRibMID, false, TEXT("WallBandY"));

		AddMeshBlock(CubeMesh, FVector(0.f, YSign * (HalfY - TraceArenaConstants::WallBandSize * 0.5f), KickZ),
			FVector(FieldLength, TraceArenaConstants::WallBandSize, TraceArenaConstants::WallBandSize),
			NeutralRibMID, false, TEXT("WallKickY"));

		// Cap raised 8 -> 12 with the 3.5:1 field: the ribs are spaced in ABSOLUTE uu (2200), so the
		// count is what grows when the field does. At HalfX 16800 this wants 7, and the old cap of 8
		// was one rib away from silently truncating the row at 17600 uu.
		const int32 RibsPerHalf = FMath::Clamp(FMath::FloorToInt(HalfX / TraceArenaConstants::WallRibSpacing), 0, 12);
		for (int32 Index = -RibsPerHalf; Index <= RibsPerHalf; ++Index)
		{
			AddMeshBlock(CubeMesh,
				FVector(Index * TraceArenaConstants::WallRibSpacing, YSign * (HalfY - TraceArenaConstants::WallRibDepth * 0.5f), RibZ),
				FVector(TraceArenaConstants::WallRibWidth, TraceArenaConstants::WallRibDepth, RibHeight),
				NeutralRibMID, false, TEXT("WallRibY"));
		}
	}

	const ETraceTeam EndTeams[] = { ETraceTeam::Blue, ETraceTeam::Orange };
	for (const ETraceTeam Team : EndTeams)
	{
		const float Sign = TeamEndSign(Team);
		const FLinearColor TeamColor = TraceTeamColor(Team);
		UMaterialInstanceDynamic* TeamTrimMID = MakeNeonMID(TeamColor, TraceArenaConstants::GlowTrim);
		UMaterialInstanceDynamic* TeamRibMID = MakeNeonMID(TeamColor, TraceArenaConstants::GlowRib);
		RegisterSideMID(Sign, TeamTrimMID, /*bNeon=*/true, TraceArenaConstants::GlowTrim);
		RegisterSideMID(Sign, TeamRibMID, /*bNeon=*/true, TraceArenaConstants::GlowRib);

		// The top strip is untagged and permanent: at WallHeight - 23 uu it sits well clear of the
		// mode-B ring aperture, and it is the line that reads the end of the field from midfield in
		// both modes.
		AddMeshBlock(CubeMesh, FVector(Sign * (HalfX - TrimInset), 0.f, TrimZ),
			FVector(TraceArenaConstants::WallTrimSize, FieldWidth, TraceArenaConstants::WallTrimSize),
			TeamTrimMID, false, TEXT("EndTrim"));

		// EVERYTHING BELOW IT IS MODE A ONLY. The band, the kick band and the vertical ribs all cross
		// the height and the width the mode-B hoop occupies, so in mode B they would be three glowing
		// bars drawn straight across the goal - the strongest lines on the wall, contradicting the
		// thing the wall is now for. In mode B the ring and its alcove are the dressing at this end.
		const int32 TrimMark = MarkBuiltComponents();

		AddMeshBlock(CubeMesh, FVector(Sign * (HalfX - TraceArenaConstants::WallBandSize * 0.5f), 0.f, BandZ),
			FVector(TraceArenaConstants::WallBandSize, FieldWidth, TraceArenaConstants::WallBandSize),
			TeamRibMID, false, TEXT("EndBand"));

		AddMeshBlock(CubeMesh, FVector(Sign * (HalfX - TraceArenaConstants::WallBandSize * 0.5f), 0.f, KickZ),
			FVector(TraceArenaConstants::WallBandSize, FieldWidth, TraceArenaConstants::WallBandSize),
			TeamRibMID, false, TEXT("EndKick"));

		const int32 RibsPerHalf = FMath::Clamp(FMath::FloorToInt(HalfY / TraceArenaConstants::WallRibSpacing), 0, 4);
		for (int32 Index = -RibsPerHalf; Index <= RibsPerHalf; ++Index)
		{
			AddMeshBlock(CubeMesh,
				FVector(Sign * (HalfX - TraceArenaConstants::WallRibDepth * 0.5f), Index * TraceArenaConstants::WallRibSpacing, RibZ),
				FVector(TraceArenaConstants::WallRibDepth, TraceArenaConstants::WallRibWidth, RibHeight),
				TeamRibMID, false, TEXT("EndRib"));
		}

		CollectPiecesSince(TrimMark, EndzoneModePieces);
	}
}

void ATraceArenaBuilder::BuildGrid()
{
	if (GridSpacing < 50.f)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("ATraceArenaBuilder: GridSpacing %.1f is too small; skipping the floor grid."), GridSpacing);
		return;
	}

	// Territory is readable off the floor: a line's colour is the colour of the team that DEFENDS the
	// half it lies in, so from anywhere on the field you can tell which way you are attacking without
	// looking up at the walls. The halfway line is neutral and much brighter.
	UMaterialInstanceDynamic* BlueGridMID = MakeNeonMID(TraceTeamColor(ETraceTeam::Blue), TraceArenaConstants::GlowGrid);
	UMaterialInstanceDynamic* OrangeGridMID = MakeNeonMID(TraceTeamColor(ETraceTeam::Orange), TraceArenaConstants::GlowGrid);
	UMaterialInstanceDynamic* CenterMID = MakeNeonMID(TraceArenaConstants::NeonNeutralPale, TraceArenaConstants::GlowCentreLine);

	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();

	// The -X half wears the colour of whoever defends the -X end. Blue at build time (TeamEndSign);
	// registered so the half-time switch repaints the floor along with everything else, because a
	// floor that still says "this is your half" after the sides swap is worse than an unpainted one.
	RegisterSideMID(-1.f, BlueGridMID, /*bNeon=*/true, TraceArenaConstants::GlowGrid);
	RegisterSideMID(1.f, OrangeGridMID, /*bNeon=*/true, TraceArenaConstants::GlowGrid);
	auto HalfColorMID = [BlueGridMID, OrangeGridMID](float X) { return (X < 0.f) ? BlueGridMID : OrangeGridMID; };

	const int32 MaxHalfLines = FMath::Max(1, MaxGridLinesPerAxis / 2);
	const int32 HalfLinesX = FMath::Min(FMath::FloorToInt(HalfX / GridSpacing), MaxHalfLines);

	// THE GRID STOPS AT THE BANK TOE. Every strip here is a 5 uu slab lying on the floor at Z = 2,
	// and the corner banks are solid boxes rising from Z = 0 - so any part of the grid out past
	// BankInnerHalfWidth() is not "a line on a slope", it is a line INSIDE a terrace, invisible and
	// paid for. Clipping the constant-X strips to the flat playfield and dropping the constant-Y
	// strips that fall outside it also makes the grid say something true: the lit rectangle is
	// exactly the flat ground, and the banks read as the frame around it.
	const float GridHalfY = BankInnerHalfWidth();
	const int32 HalfLinesY = FMath::Min(FMath::FloorToInt(GridHalfY / GridSpacing), MaxHalfLines);

	// Lines of constant X, running the width of the flat playfield. Each lies entirely in one half,
	// so it takes that half's colour outright.
	for (int32 Index = -HalfLinesX; Index <= HalfLinesX; ++Index)
	{
		const float X = Index * GridSpacing;
		if (FMath::IsNearlyEqual(FMath::Abs(X), HalfX, 1.f))
		{
			continue; // That is the wall, not a grid line.
		}

		const bool bCenter = (Index == 0);
		AddMeshBlock(CubeMesh,
			FVector(X, 0.f, TraceArenaConstants::GridZ),
			FVector(bCenter ? GridStripWidth * 3.f : GridStripWidth, GridHalfY * 2.f, TraceArenaConstants::GridThickness),
			bCenter ? CenterMID : HalfColorMID(X),
			/*bCastShadow=*/false,
			TEXT("GridX"));
	}

	// Lines of constant Y run the whole length and therefore cross both halves, so each one is built
	// as two half-length strips rather than one - that is what keeps the territory read consistent.
	for (int32 Index = -HalfLinesY; Index <= HalfLinesY; ++Index)
	{
		const float Y = Index * GridSpacing;
		if (FMath::IsNearlyEqual(FMath::Abs(Y), HalfY, 1.f) || FMath::Abs(Y) > GridHalfY)
		{
			continue;
		}

		for (const float XSign : { -1.f, 1.f })
		{
			AddMeshBlock(CubeMesh,
				FVector(XSign * HalfX * 0.5f, Y, TraceArenaConstants::GridZ),
				FVector(HalfX, GridStripWidth, TraceArenaConstants::GridThickness),
				HalfColorMID(XSign),
				/*bCastShadow=*/false,
				TEXT("GridY"));
		}
	}
}

void ATraceArenaBuilder::BuildCentreDais(bool bBuildVisuals)
{
	const float HalfY = HalfWidth();

	// Emissive 0.012, NOT the 0.035 the rest of the structure carries, and this one is worth a note.
	//
	// The dais is the only large UP-FACING surface a player ever stands on, and up-facing surfaces
	// also catch the most key light - so the two terms stack exactly here and nowhere else. MEASURED
	// from a first-person screenshot taken standing on the top tier: at 0.035 the whole lower half of
	// the frame was a flat pale cyan sheet with no discernible surface, which is precisely the
	// "platform tops blow out to pale grey" failure recorded in the palette comment. That failure was
	// invisible while the camera was in third person and looking down at the dais from outside it;
	// the first-person camera put the player's eyeline 160 uu above it and made it unmissable.
	//
	// The tier lips do all the shape reading here anyway - four concentric glowing rims is more edge
	// per square metre than anything else in the arena.
	UMaterialInstanceDynamic* BodyMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.f, TraceArenaConstants::NeonNeutral, 0.012f)
		: nullptr;
	UMaterialInstanceDynamic* NeonMID = bBuildVisuals
		? MakeNeonMID(TraceArenaConstants::NeonNeutralPale, TraceArenaConstants::GlowLip)
		: nullptr;

	if (bBuildVisuals)
	{
		// A glowing RING on the floor marking the objective zone.
		//
		// It was a filled disc of this diameter to begin with, at a deliberately sub-1 Glow so it
		// would "pool light" rather than bloom. Measured from a screenshot taken walking up to the
		// dais: a 5200 uu emissive disc is 27 square metres of flat cyan and it drowned the entire
		// lower half of the frame, turning the near-black floor the whole art direction depends on
		// into a bright teal sheet. A ring does the same job - it tells you where the contested zone
		// starts - and leaves the floor alone.
		//
		// Built as two coaxial discs rather than a ring mesh (the engine basic shapes have no torus):
		// a neon disc with a slightly smaller floor-coloured disc laid a hair above it. Two draws,
		// exact control of the ring width, no new asset.
		const float PadDiameter = HalfY * TraceArenaConstants::CentrePadDiameterFrac * 2.f;
		UMaterialInstanceDynamic* PadMID = MakeNeonMID(TraceArenaConstants::NeonNeutral, TraceArenaConstants::GlowRing);
		AddMeshBlock(CylinderMesh, FVector(0.f, 0.f, TraceArenaConstants::PadZ),
			FVector(PadDiameter, PadDiameter, TraceArenaConstants::GridThickness),
			PadMID, /*bCastShadow=*/false, TEXT("CentreRing"));

		const float InnerDiameter = PadDiameter - TraceArenaConstants::CentreRingWidth * 2.f;
		UMaterialInstanceDynamic* MaskMID = MakeSurfaceMID(TraceArenaConstants::FloorColor, 0.16f);
		AddMeshBlock(CylinderMesh, FVector(0.f, 0.f, TraceArenaConstants::PadZ + 1.5f),
			FVector(InnerDiameter, InnerDiameter, TraceArenaConstants::GridThickness),
			MaskMID, /*bCastShadow=*/false, TEXT("CentreRingMask"));
	}

	// THE SMALL DIAMOND AT THE EXACT CENTRE, straight off the sketch: three 40 uu steps, rotated 45
	// degrees so it reads as a diamond and so a player running at it from any of the eight compass
	// directions meets a flat face square-on rather than a corner. 1900 uu across the base, 120 uu
	// tall. It is walkable from every side (see AddSteppedPlatform), which is what keeps the
	// objective reachable without a ramp for the navmesh-less bots to find.
	AddSteppedPlatform(FVector2D::ZeroVector,
		TraceArenaConstants::DaisTopTierSide, TraceArenaConstants::DaisTierSideStep,
		TraceArenaConstants::DaisTiers, 45.f, BodyMID, NeonMID, TEXT("Dais"));

	// --- Pedestal --------------------------------------------------------------------------------

	const float TopZ = DaisTopZ();
	const FVector PedestalSize(
		TraceArenaConstants::PedestalDiameter,
		TraceArenaConstants::PedestalDiameter,
		TraceArenaConstants::PedestalHeight);
	const FVector PedestalCenter(0.f, 0.f, TopZ + TraceArenaConstants::PedestalHeight * 0.5f);

	// A box under a round pedestal: it is a hair wider at the corners than the mesh, which only
	// means the Core comes to rest fractionally early. Cheaper and more predictable than convex
	// collision, and the Core is the only thing that ever touches it.
	if (UBoxComponent* PedestalCollision = AddCollisionBlock(PedestalCenter, PedestalSize * FVector(0.94f, 0.94f, 1.f), TEXT("PedestalCollision")))
	{
		// THE PEDESTAL MUST NOT BLOCK PAWNS. It sits exactly where the Core lives, and a pawn-blocking
		// pedestal is how the centre of the map becomes a place players bounce off.
		//
		//     pedestal collision half-extent  = 300 * 0.94 / 2 = 141 uu
		//   + character capsule radius        =                   34 uu
		//   = closest a player can stand      =                  175 uu from the centre
		//
		// HISTORICAL NOTE, because the measurement is still the reason this line exists. Back when the
		// Core was a physical object with a pickup sphere (UTraceSettings::PickupRadius, 110 uu — both
		// the setting and the mechanic are now deleted), 175 > 110 meant the sphere could never
		// overlap a capsule while the Core was at home, and the objective was literally untakeable.
		// That was measured, not reasoned about: ten bots walked to the centre, orbited at exactly
		// X = +/-175.03, and logged zero pickups. The Core is a status now and is granted rather than
		// touched, so that specific failure cannot recur — but the pedestal is still 60 uu tall
		// against a 45 uu MaxStepHeight, so a blocking pedestal would still be an obstacle parked on
		// the objective.
		//
		// Ignoring ECC_Pawn keeps everything the block was actually for — ECC_Visibility still blocks
		// hitscan and bot line-of-sight, ECC_Camera still blocks the spring arm — and only removes
		// the one response that made the centre hostile. Waist-high scenery you can walk through
		// beats an unwinnable match.
		PedestalCollision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}

	if (!bBuildVisuals)
	{
		return;
	}

	// Matte and near-black like every other structure, with a bright ring around its rim.
	//
	// It was previously glossy (roughness 0.34, metallic 0.2) over a much lighter albedo, and a
	// screenshot taken standing on the dais showed exactly what that gets you: a plain light-grey
	// disc sitting in the middle of an otherwise black-and-neon arena, the one object still reading
	// as untextured greybox. The plinth the objective rests on should be the most deliberate-looking
	// thing on the field, so the shape now comes from the ring, not from the shading.
	UMaterialInstanceDynamic* PedestalMID = MakeSurfaceMID(TraceArenaConstants::PedestalColor, 0.48f, 0.f,
		TraceArenaConstants::NeonNeutralPale, 0.06f);
	AddMeshBlock(CylinderMesh, PedestalCenter, PedestalSize, PedestalMID, /*bCastShadow=*/true, TEXT("Pedestal"));

	UMaterialInstanceDynamic* PedestalRingMID = MakeNeonMID(TraceArenaConstants::NeonNeutralPale, TraceArenaConstants::GlowTrim);
	const float RimDiameter = TraceArenaConstants::PedestalDiameter + 44.f;
	AddMeshBlock(CylinderMesh,
		FVector(0.f, 0.f, TopZ + TraceArenaConstants::PedestalHeight - 14.f),
		FVector(RimDiameter, RimDiameter, 20.f),
		PedestalRingMID, /*bCastShadow=*/false, TEXT("PedestalRim"));
	AddMeshBlock(CylinderMesh,
		FVector(0.f, 0.f, TopZ + TraceArenaConstants::GridThickness * 0.5f),
		FVector(RimDiameter + 90.f, RimDiameter + 90.f, TraceArenaConstants::GridThickness),
		PedestalRingMID, /*bCastShadow=*/false, TEXT("PedestalBase"));

	// Four light pylons standing around the objective. They are the tallest interior structures and
	// they are what makes the centre findable from the far end of a 33600 uu field.
	UMaterialInstanceDynamic* PylonBodyMID = MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.f,
		TraceArenaConstants::NeonNeutral, 0.026f);
	UMaterialInstanceDynamic* PylonNeonMID = MakeNeonMID(TraceArenaConstants::NeonNeutral, TraceArenaConstants::GlowPylon);

	for (const float XSign : { -1.f, 1.f })
	{
		for (const float YSign : { -1.f, 1.f })
		{
			AddPylon(FVector2D(XSign * HalfLength() * TraceArenaConstants::DaisPylonXFrac,
					YSign * HalfY * TraceArenaConstants::DaisPylonYFrac),
				TraceArenaConstants::DaisPylonSide, TraceArenaConstants::DaisPylonHeight,
				PylonBodyMID, PylonNeonMID, TEXT("DaisPylon"));
		}
	}
}

void ATraceArenaBuilder::BuildCornerBanks(bool bBuildVisuals)
{
	// ---------------------------------------------------------------------------------------------
	// THE SHALLOW STADIUM BOWL - spec v3 section 7, the sketch's green arrows.
	//
	// Four banks, one per quadrant. Each is a nest of solid boxes rising from Z = 0, terrace 0
	// widest/longest/lowest and terrace N-1 smallest/highest, so the top surface is a staircase that
	// descends toward the centre of the field in BOTH axes:
	//
	//        goal line                      halfway
	//    |Y| = 4800  ###############################____        <- terrace N-1 crest, 352 uu
	//                  ###########################______
	//                    #######################__________
	//    |Y| = 3300        ###################______________    <- terrace 0 toe, 39 uu
	//                          flat central playfield
	//
	// EVERY RISER IS UNDER MaxStepHeight, and that is the load-bearing property. The terrace count
	// is DERIVED from BankHeight rather than fixed, so raising the bank adds terraces instead of
	// making the risers taller - a bank that "just" needed a taller step would silently become a
	// wall, and a wall along both sidelines is a bot trap and a carrier's dead end at once.
	//
	// WHY THE BANKS STOP AT THE GOAL LINE. The endzones stay flat and full width. The goal line
	// decal, the endzone floor patch, the gate towers, the spawn fan and the endzone respawn pads
	// all assume a floor at Z = 0 out there, and a scoring volume you have to climb into is a rule
	// nobody asked for. The bank's crest sits just inboard of the goal line and steps back down over
	// BankGoalSetback uu, so the high ground overlooks the endzone rather than being part of it.
	// ---------------------------------------------------------------------------------------------

	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();
	const float Depth = HalfY - BankInnerHalfWidth();
	const float Height = FMath::Max(0.f, BankHeight);

	if (Depth <= 1.f || Height <= 1.f)
	{
		return;
	}

	// Terrace count from the riser ceiling, not the other way round. Ceil, so the riser is always
	// STRICTLY under StepRise (which is itself 5 uu under the engine's MaxStepHeight of 45).
	const int32 TerraceCount = FMath::Clamp(FMath::CeilToInt(Height / TraceArenaConstants::StepRise), 1, 24);
	const float Riser = Height / static_cast<float>(TerraceCount);

	// The goal line is where the bank has to stop. GoalLineX() rather than a hand-rolled
	// HalfX - EndzoneDepth, for the reason spelled out on ClampedEndzoneDepth(): three hand-written
	// copies of this clamp is how a spawn pad ended up on the wrong side of a goal line.
	const float GoalX = GoalLineX();
	const float Inboard = FMath::Max(0.f, GoalX * TraceArenaConstants::BankInboardTaperFrac);
	const float Setback = FMath::Min(TraceArenaConstants::BankGoalSetback, FMath::Max(0.f, GoalX * 0.5f));

	// Emissive 0.012, the DAIS number, not the 0.026-0.030 the cover blocks carry - and for exactly
	// the reason recorded on the dais: a terrace is a large UP-FACING surface, up-facing surfaces
	// also catch the most key light, and the two terms stack. The palette comment one screen up
	// records what that costs (platform tops blowing out to a flat pale sheet at ~200/255, judged
	// from a screenshot of a platform top rather than of a wall), and the banks are by area the
	// largest up-facing structure in the arena - four of them, 1500 uu deep, the full half length.
	// The per-terrace lips do all the shape reading here anyway: nine concentric glowing contours
	// per bank is more edge per square metre than anything except the dais itself.
	UMaterialInstanceDynamic* BodyMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.f,
			TraceArenaConstants::NeonNeutral, 0.012f)
		: nullptr;

	for (const float XSign : { -1.f, 1.f })
	{
		// The bank wears the colour of the half it lies in, exactly like the floor grid and the
		// flank dressing: a player who has lost their bearings on a bank can still read which way
		// they are attacking off the contour lines under their feet.
		const ETraceTeam HalfTeam = (XSign < 0.f) ? ETraceTeam::Blue : ETraceTeam::Orange;
		UMaterialInstanceDynamic* NeonMID = bBuildVisuals
			? MakeNeonMID(TraceTeamColor(HalfTeam), TraceArenaConstants::GlowLip)
			: nullptr;

		for (const float YSign : { -1.f, 1.f })
		{
			for (int32 Terrace = 0; Terrace < TerraceCount; ++Terrace)
			{
				const float Alpha = static_cast<float>(Terrace) / static_cast<float>(TerraceCount);
				const float TopZ = Riser * static_cast<float>(Terrace + 1);

				// Y: terrace 0 reaches BankDepth in from the sideline, each one after it a step less,
				// so the sloped strip is Depth wide with TerraceCount treads across it.
				const float InnerY = HalfY - Depth * (1.f - Alpha);

				// X: terrace 0 runs from the halfway line to just short of the goal line; higher
				// terraces start further out and stop further back, which is what turns a trough
				// along the sideline into a bank that is highest at the CORNER.
				const float NearX = Inboard * Alpha;
				const float FarX = GoalX - TraceArenaConstants::BankGoalClearance - Setback * Alpha;

				const float SpanX = FarX - NearX;
				const float SpanY = HalfY - InnerY;
				if (SpanX <= 1.f || SpanY <= 1.f)
				{
					continue;
				}

				// bVerticalTrim = false: a 39 uu riser would carry four 39 uu corner-rib stubs.
				// bFaceBands = false: the terraces are NESTED, so a skirt on this one would be
				// buried inside the solid body of the one outside it. The top lip alone gives each
				// terrace one glowing contour line, which is the whole read - and its 12 uu
				// protrusion hangs 13 uu above the tread below, so it never blocks a foot.
				AddNeonBlock(
					FVector(XSign * (NearX + SpanX * 0.5f), YSign * (InnerY + SpanY * 0.5f), TopZ * 0.5f),
					FVector(SpanX, SpanY, TopZ),
					/*YawDegrees=*/0.f, BodyMID, NeonMID, /*bCollide=*/true, TEXT("Bank"),
					/*FaceNeonMID=*/nullptr, /*bVerticalTrim=*/false, /*bFaceBands=*/false);
			}
		}
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("Corner banks: %d terraces per bank, %.1f uu riser (ceiling %.0f), crest %.0f uu, %.0f uu deep, X %.0f..%.0f."),
		TerraceCount, Riser, TraceArenaConstants::StepRise, Height, Depth,
		0.f, GoalX - TraceArenaConstants::BankGoalClearance);
}

void ATraceArenaBuilder::BuildCoverField(bool bBuildVisuals)
{
	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();

	// A faint self-lit tint on the body itself, on top of the face trim.
	//
	// Cover blocks are the one structure a player ends up nose-to-nose with, and at that range the
	// trim is off the edges of the frame - what fills the screen is the middle of a face. Emissive is
	// the only term that survives there, because it does not depend on an incidence angle the way
	// every directional light in the rig does.
	//
	// MEASURED, twice. 0.055 reproduced the exact failure the palette comment warns about one screen
	// up: every face came back a flat mid-slate and the cover blocks read as frosted glass panels
	// rather than as solid matter. 0.02 went too far the other way and a point-blank face was black
	// again. The tonemapper's toe is very steep down here, so these are not small differences - a
	// factor of three in emissive is far more than a factor of three on screen. 0.03 lifts a face
	// just off the void while leaving the shading gradient from the lights visible on top of it,
	// which is what makes it look like a surface rather than like a panel.
	UMaterialInstanceDynamic* BodyMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.05f,
			TraceArenaConstants::NeonNeutral, 0.030f)
		: nullptr;

	// ONE PLAYER HEIGHT, read off the capsule. Every block below is exactly 1x, 2x or 3.5x of it -
	// the sketch's green, orange and red outlines - and nothing in between. That is what makes the
	// arena readable: a block is either something you can shoot over, something you can hide behind,
	// or a landmark, and the three are told apart at a glance by height alone.
	const float PlayerHeight = PlayerHeightUU();

	for (const float XSign : { -1.f, 1.f })
	{
		const ETraceTeam HalfTeam = (XSign < 0.f) ? ETraceTeam::Blue : ETraceTeam::Orange;
		UMaterialInstanceDynamic* NeonMID = bBuildVisuals
			? MakeNeonMID(TraceTeamColor(HalfTeam), TraceArenaConstants::GlowLip)
			: nullptr;
		UMaterialInstanceDynamic* FaceMID = bBuildVisuals
			? MakeNeonMID(TraceTeamColor(HalfTeam), TraceArenaConstants::GlowFace)
			: nullptr;

		// MIRRORED IN X. The sketch's two halves are different; this builds the same half twice,
		// because the match switches sides at half time and an asymmetric field would hand one team
		// the better ground for a full half. See the header. Also mirrored in Y, which the sketch's
		// own scatter is roughly but not exactly - same argument, smaller stakes.
		//
		// Diamonds where the spec says diamond: a pawn (or a bot steering straight at a target
		// behind it) that meets a 45-degree face gets deflected around the block by the movement
		// component's own wall sliding, instead of standing still pushing into a flat face until
		// stuck-detection fires. The long bars are left axis-aligned - they are meant to be run
		// ALONG, and a yawed bar would just be a diagonal wall across a lane.
		//
		// One lambda for both lists, because the ONLY difference between them is how the X anchor
		// resolves - see the two-anchors note in TraceArenaConstants.
		auto PlaceCover = [&](const TraceArenaConstants::FCoverSpec& Spec, float LocalX)
		{
			const float BlockHeight = PlayerHeight * Spec.HeightMult;

			for (const float YSign : { -1.f, 1.f })
			{
				const FVector Centre(XSign * LocalX, YSign * HalfY * Spec.YFrac, BlockHeight * 0.5f);

				// A yawed bar has to be mirrored by NEGATING the yaw, not by reusing it: reflecting a
				// shape through an axis reverses its handedness. Every entry is currently 0 or 45 on
				// a square footprint, where it makes no difference - but the day someone adds the
				// sketch's diagonal, a shared yaw would quietly build two blocks that are rotations
				// of each other rather than mirror images, and the two halves would stop matching.
				const float Yaw = Spec.Yaw * XSign * YSign;

				AddNeonBlock(Centre, FVector(Spec.SizeX, Spec.SizeY, BlockHeight), Yaw,
					BodyMID, NeonMID, /*bCollide=*/true, TEXT("Cover"), FaceMID);
			}
		};

		// The goal approach: measured back from the goal line, so it keeps its tuned relationship to
		// the goal, the gate and the spawn fan however long the field is.
		const float GoalX = GoalLineX();
		for (const TraceArenaConstants::FCoverSpec& Spec : TraceArenaConstants::ApproachCover)
		{
			// Never let a deep endzone or a short field push an approach block through the centre
			// line into the other team's half - that would break the mirror, not just the spacing.
			PlaceCover(Spec, FMath::Max(0.f, GoalX - Spec.XAnchor));
		}

		// The midfield: a fraction of the span from the centre line out to the innermost approach
		// block, so THIS is the part of the layout that stretches when the field is lengthened.
		const TraceArenaConstants::FCoverSpec& Innermost =
			TraceArenaConstants::ApproachCover[UE_ARRAY_COUNT(TraceArenaConstants::ApproachCover) - 1];
		const float MidfieldSpan = FMath::Max(0.f, GoalX - Innermost.XAnchor);

		for (const TraceArenaConstants::FCoverSpec& Spec : TraceArenaConstants::MidfieldCover)
		{
			PlaceCover(Spec, MidfieldSpan * Spec.XAnchor);
		}

		// Long 2x bars straddling the centreline of each half, so the shortest route from the centre
		// diamond to a goal is never a clear straight line. Two of them on the 3.5:1 field - see
		// AxisCoverXFracs.
		const float AxisHeight = PlayerHeight * TraceArenaConstants::StructureHeight2x;
		for (const float AxisXFrac : TraceArenaConstants::AxisCoverXFracs)
		{
			AddNeonBlock(FVector(XSign * HalfX * AxisXFrac, 0.f, AxisHeight * 0.5f),
				FVector(TraceArenaConstants::AxisCoverSizeX, TraceArenaConstants::AxisCoverSizeY, AxisHeight),
				0.f, BodyMID, NeonMID, /*bCollide=*/true, TEXT("AxisCover"), FaceMID);
		}
	}

	// --- The 3.5x tower at top centre ------------------------------------------------------------
	//
	// On the dividing line, off to the +Y side, exactly as drawn. Neutral cyan rather than a team
	// colour because it belongs to neither half - the same call the floor grid's centre strip and
	// the midfield buttress already make.
	UMaterialInstanceDynamic* TowerNeonMID = bBuildVisuals
		? MakeNeonMID(TraceArenaConstants::NeonNeutralPale, TraceArenaConstants::GlowLip)
		: nullptr;
	UMaterialInstanceDynamic* TowerFaceMID = bBuildVisuals
		? MakeNeonMID(TraceArenaConstants::NeonNeutral, TraceArenaConstants::GlowFace)
		: nullptr;

	const float TowerHeight = PlayerHeight * TraceArenaConstants::StructureHeight35x;
	const float TowerSide = TraceArenaConstants::TopCentreTowerSide;
	AddNeonBlock(
		FVector(0.f, HalfY * TraceArenaConstants::TopCentreTowerYFrac, TowerHeight * 0.5f),
		FVector(TowerSide, TowerSide, TowerHeight), 45.f,
		BodyMID, TowerNeonMID, /*bCollide=*/true, TEXT("TopCentreTower"), TowerFaceMID);
}

void ATraceArenaBuilder::BuildFlanks(bool bBuildVisuals)
{
	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();

	// One shared body material: buttresses are dark structure like everything else, with the same
	// faint self-lit tint the cover blocks got, because they are also things you end up standing
	// right next to.
	UMaterialInstanceDynamic* BodyMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.52f, 0.f,
			TraceArenaConstants::NeonNeutral, 0.026f)
		: nullptr;

	// Per-half neon, so the flank dressing carries the same territory read as the floor grid and the
	// walls: everything in the -X half is blue, everything in the +X half is orange. A player who has
	// lost their bearings in a corner can work out which way they are attacking from the wall alone.
	UMaterialInstanceDynamic* HalfNeon[2] = { nullptr, nullptr };
	UMaterialInstanceDynamic* HalfBridge[2] = { nullptr, nullptr };
	UMaterialInstanceDynamic* HalfStripe[2] = { nullptr, nullptr };
	if (bBuildVisuals)
	{
		const ETraceTeam HalfTeams[2] = { ETraceTeam::Blue, ETraceTeam::Orange };   // index 0 = -X
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const FLinearColor Color = TraceTeamColor(HalfTeams[Index]);
			HalfNeon[Index] = MakeNeonMID(Color, TraceArenaConstants::GlowPylon);
			HalfBridge[Index] = MakeNeonMID(Color, TraceArenaConstants::GlowBridge);
			HalfStripe[Index] = MakeNeonMID(Color, TraceArenaConstants::GlowLaneStripe);
		}
	}
	auto HalfIndex = [](float XSign) { return (XSign < 0.f) ? 0 : 1; };

	// --- Buttresses ------------------------------------------------------------------------------

	for (const float YSign : { -1.f, 1.f })
	{
		for (const TraceArenaConstants::FButtressSpec& Spec : TraceArenaConstants::SideButtresses)
		{
			// PositionFrac 0 lands on the centreline and must be built once, not twice.
			const bool bOnAxis = FMath::IsNearlyZero(Spec.PositionFrac);
			for (const float XSign : { -1.f, 1.f })
			{
				if (bOnAxis && XSign > 0.f)
				{
					continue;
				}

				const float X = XSign * HalfX * Spec.PositionFrac;
				const float Y = YSign * (HalfY - TraceArenaConstants::ButtressDepth * 0.5f);

				// On the centreline the half is ambiguous; neutral cyan marks it as the halfway line,
				// which is exactly what the floor grid does with its own centre strip.
				UMaterialInstanceDynamic* NeonMID = bOnAxis
					? (bBuildVisuals ? MakeNeonMID(TraceArenaConstants::NeonNeutralPale, TraceArenaConstants::GlowPylon) : nullptr)
					: HalfNeon[HalfIndex(XSign)];

				AddWallButtress(FVector2D(X, Y), FVector2D(0.f, -YSign),
					TraceArenaConstants::ButtressWidth, TraceArenaConstants::ButtressDepth, Spec.Height,
					BodyMID, NeonMID, TEXT("SideButtress"));
			}
		}
	}

	for (const float XSign : { -1.f, 1.f })
	{
		for (const TraceArenaConstants::FButtressSpec& Spec : TraceArenaConstants::EndButtresses)
		{
			for (const float YSign : { -1.f, 1.f })
			{
				const float X = XSign * (HalfX - TraceArenaConstants::ButtressDepth * 0.5f);
				const float Y = YSign * HalfY * Spec.PositionFrac;

				// Rotated 90 degrees: the width runs along Y and the depth into the end wall.
				AddWallButtress(FVector2D(X, Y), FVector2D(-XSign, 0.f),
					TraceArenaConstants::ButtressWidth, TraceArenaConstants::ButtressDepth, Spec.Height,
					BodyMID, HalfNeon[HalfIndex(XSign)], TEXT("EndButtress"));
			}
		}
	}

	// --- Corner pylons ---------------------------------------------------------------------------
	//
	// The four corners behind the goal lines were the only part of the field with no structure of any
	// kind, the endzone floor being otherwise flat - and it is still flat, because the corner banks
	// deliberately stop at the goal line. A tall column in each corner closes the room off and,
	// incidentally, gives a defender something to fight around. They stand at the MIDDLE of the
	// endzone (X = 15600 on the shipped field) rather than at a fraction of the half length - see
	// CornerPylonYFrac for why that distinction cost a gate tower 80 uu of clearance.
	//
	// Checked on the 33600 field: (15600, 4080) with a 300 uu side leaves 840 uu to the gate tower
	// face at X = 14610, 1050 uu to the end wall, and 1200 uu in Y to the outermost endzone respawn
	// pad at (15600, 2880).
	//
	// Built BEFORE the visuals-only section on purpose: these block, and a dedicated server has to
	// build the same collision the clients are predicting against.
	const float CornerPylonX = HalfX - ClampedEndzoneDepth() * 0.5f;
	for (const float XSign : { -1.f, 1.f })
	{
		for (const float YSign : { -1.f, 1.f })
		{
			AddPylon(FVector2D(XSign * CornerPylonX, YSign * HalfY * TraceArenaConstants::CornerPylonYFrac),
				TraceArenaConstants::CornerPylonSide, TraceArenaConstants::CornerPylonHeight,
				BodyMID, HalfNeon[HalfIndex(XSign)], TEXT("CornerPylon"));
		}
	}

	// --- Lane pylons -----------------------------------------------------------------------------
	//
	// Two per quadrant on the 3.5:1 field, marking the flank route and each carrying a light bridge
	// out to the wall. They used to be built alongside the wing platforms; the wings are gone (the
	// banks replaced them) so they live here now, with the rest of the flank dressing they belong to.
	// Collision, so they are outside the bBuildVisuals gate with everything else that blocks.
	for (const float XSign : { -1.f, 1.f })
	{
		for (const float XFrac : TraceArenaConstants::LanePylonXFracs)
		{
			for (const float YSign : { -1.f, 1.f })
			{
				AddPylon(FVector2D(XSign * HalfX * XFrac, YSign * HalfY * TraceArenaConstants::LanePylonYFrac),
					TraceArenaConstants::LanePylonSide, TraceArenaConstants::LanePylonHeight,
					BodyMID, HalfNeon[HalfIndex(XSign)], TEXT("LanePylon"));
			}
		}
	}

	if (!bBuildVisuals)
	{
		return;
	}

	// --- High rail -------------------------------------------------------------------------------
	//
	// The single most valuable piece here. A frame looking down a flank is mostly SKY - everything
	// built on the deck is below the horizon line and the wall trim is 2600 uu up and a long way off,
	// so the middle band of the image had nothing in it at all. A continuous glowing line at 1640 uu,
	// close to the camera and running to the vanishing point, fills that band and gives the eye
	// something to measure travel against. Built as two half-length pieces so each half keeps its
	// own colour.
	for (const float YSign : { -1.f, 1.f })
	{
		for (const float XSign : { -1.f, 1.f })
		{
			AddMeshBlock(CubeMesh,
				FVector(XSign * HalfX * 0.5f,
					YSign * (HalfY - TraceArenaConstants::ButtressDepth - TraceArenaConstants::FlankRailSize * 0.5f),
					TraceArenaConstants::FlankRailZ),
				FVector(HalfX, TraceArenaConstants::FlankRailSize, TraceArenaConstants::FlankRailSize),
				HalfBridge[HalfIndex(XSign)], /*bCastShadow=*/false, TEXT("FlankRail"));
		}
	}

	// --- Light bridges ---------------------------------------------------------------------------
	//
	// One per quadrant, from the top of the lane pylon out to the side wall. The pylons were already
	// the tallest thing in the outer lane and they read as loose columns standing in nothing; tying
	// them to the wall turns the flank into a structure with a span over it, and the beam crosses the
	// outer lane at 1240 uu so it frames anyone running underneath.
	const float BridgeZ = TraceArenaConstants::LanePylonHeight - TraceArenaConstants::BridgeDrop;
	for (const float XSign : { -1.f, 1.f })
	{
		for (const float XFrac : TraceArenaConstants::LanePylonXFracs)
		{
			for (const float YSign : { -1.f, 1.f })
			{
				const float PylonY = YSign * HalfY * TraceArenaConstants::LanePylonYFrac;
				const float WallY = YSign * (HalfY - TraceArenaConstants::ButtressDepth);
				const float Span = FMath::Abs(WallY - PylonY);

				AddMeshBlock(CubeMesh,
					FVector(XSign * HalfX * XFrac, (PylonY + WallY) * 0.5f, BridgeZ),
					FVector(TraceArenaConstants::BridgeSize, Span, TraceArenaConstants::BridgeSize),
					HalfBridge[HalfIndex(XSign)], /*bCastShadow=*/false, TEXT("LightBridge"));
			}
		}
	}

	// --- Outer-lane floor stripes ----------------------------------------------------------------
	//
	// Two bright lines per side, running the length of each half. The floor grid already draws lines
	// out here but at GlowGrid (1.5) they are a whisper; these are lane markings, and the near-mirror
	// floor doubles every one of them into the black space underneath. This is what stops the bottom
	// third of a flank frame being empty.
	for (const float YSign : { -1.f, 1.f })
	{
		for (const float YFrac : TraceArenaConstants::LaneStripeYFracs)
		{
			for (const float XSign : { -1.f, 1.f })
			{
				AddMeshBlock(CubeMesh,
					FVector(XSign * HalfX * 0.5f, YSign * HalfY * YFrac, TraceArenaConstants::GoalLineZ),
					FVector(HalfX, TraceArenaConstants::LaneStripeWidth, TraceArenaConstants::GoalLineThickness),
					HalfStripe[HalfIndex(XSign)], /*bCastShadow=*/false, TEXT("LaneStripe"));
			}
		}
	}

}

void ATraceArenaBuilder::BuildEndzones(bool bBuildVisuals)
{
	UWorld* World = GetWorld();
	const float HalfX = HalfLength();

	// FULL WIDTH, from one function, for every piece of this. The trigger, the floor patch, the goal
	// line and the gate all measure themselves against EndzoneHalfWidth() so the volume that scores
	// and the paint that advertises it are the same rectangle by construction rather than by
	// coincidence. See the header note on EndzoneHalfWidth before narrowing anything here.
	const float HalfY = EndzoneHalfWidth();
	const float ZoneWidth = HalfY * 2.f;
	const float Depth = ClampedEndzoneDepth();

	const ETraceTeam Teams[] = { ETraceTeam::Blue, ETraceTeam::Orange };
	for (const ETraceTeam Team : Teams)
	{
		const float Sign = TeamEndSign(Team);
		const float CenterX = Sign * (HalfX - Depth * 0.5f);
		const float GoalX = Sign * GoalLineX();
		const FLinearColor TeamColor = TraceTeamColor(Team);

		if (bBuildVisuals)
		{
			// --- Mode-A-only paint ---------------------------------------------------------------
			//
			// The full-width patch and the two sideline rails are the pieces that CLAIM the whole
			// width scores, and in mode B that claim is false, so they are tagged and hidden there.
			// The goal LINE and the gate above are deliberately NOT tagged: the line is where the
			// endzone begins in both modes (the mode-B goal stands on it), and the gate is the
			// "you are attacking that way" landmark that a 33600 uu field needs whichever game is
			// being played. Hiding those two as well left mode B's end of the field unlit and
			// unreadable, which is a bigger lie than an over-generous floor patch.
			const int32 EndzonePaintMark = MarkBuiltComponents();

			// The patch is tinted with the colour of the team that DEFENDS this end. Their opponent
			// is the one who scores on it - see ATraceEndzone's class comment. It is a lit surface
			// with a faint emissive term rather than a neon block, so it reads as a floor that glows
			// rather than as a light panel, and the goal line in front of it stays the bright thing.
			UMaterialInstanceDynamic* PatchMID = MakeSurfaceMID(
				TraceArenaConstants::Dim(TeamColor, 0.020f), 0.20f, 0.f, TeamColor, 0.10f);
			RegisterSideMID(Sign, PatchMID, /*bNeon=*/false, /*Intensity=*/0.10f, /*BaseDim=*/0.020f);
			AddMeshBlock(CubeMesh,
				FVector(CenterX, 0.f, TraceArenaConstants::PatchZ),
				FVector(Depth, ZoneWidth, TraceArenaConstants::PatchThickness),
				PatchMID, /*bCastShadow=*/false, TEXT("EndzonePatch"));

			UMaterialInstanceDynamic* LineMID = MakeNeonMID(TeamColor, TraceArenaConstants::GlowGoalLine);
			RegisterSideMID(Sign, LineMID, /*bNeon=*/true, TraceArenaConstants::GlowGoalLine);

			// Two rails on the floor running from the goal line back to the end wall, one against
			// each sideline, closing the endzone off visually so it reads as a room you score into
			// rather than as more floor. They sit ON the sidelines because the zone reaches them:
			// together with the goal line they draw the exact rectangle the trigger occupies.
			for (const float YSign : { -1.f, 1.f })
			{
				AddMeshBlock(CubeMesh,
					FVector(CenterX, YSign * (HalfY - TraceArenaConstants::GoalLineWidth * 0.5f), TraceArenaConstants::GoalLineZ),
					FVector(Depth, TraceArenaConstants::GoalLineWidth, TraceArenaConstants::GoalLineThickness),
					LineMID, /*bCastShadow=*/false, TEXT("EndzoneEdge"));
			}

			CollectPiecesSince(EndzonePaintMark, EndzoneModePieces);

			// Shared from here down.
			AddMeshBlock(CubeMesh,
				FVector(GoalX, 0.f, TraceArenaConstants::GoalLineZ),
				FVector(TraceArenaConstants::GoalLineWidth, ZoneWidth, TraceArenaConstants::GoalLineThickness),
				LineMID, /*bCastShadow=*/false, TEXT("GoalLine"));

			// --- Gate ----------------------------------------------------------------------------
			//
			// Two towers ON THE SIDELINES with a beam across the entire width between them. This is
			// the single biggest piece of "you are attacking THAT way" signage in the arena: 2600 uu
			// tall, lit in the defending team's colour, visible from the opposite endzone - and now
			// also the thing that tells you how WIDE the endzone is, which is the whole point of
			// moving the towers out. A gate you can run round is a gate that lies about the zone.
			UMaterialInstanceDynamic* GateBodyMID = MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.f,
				TeamColor, 0.026f);
			UMaterialInstanceDynamic* GateNeonMID = MakeNeonMID(TeamColor, TraceArenaConstants::GlowPylon);
			RegisterSideMID(Sign, GateBodyMID, /*bNeon=*/false, /*Intensity=*/0.026f, /*BaseDim=*/-1.f);
			RegisterSideMID(Sign, GateNeonMID, /*bNeon=*/true, TraceArenaConstants::GlowPylon);

			// Outer face GateTowerWallGap off the side wall; Max() keeps a silly FieldWidth from
			// pushing the two towers through each other at the centreline.
			const float TowerY = FMath::Max(TraceArenaConstants::GateTowerSide,
				HalfY - TraceArenaConstants::GateTowerWallGap - TraceArenaConstants::GateTowerSide * 0.5f);
			for (const float YSign : { -1.f, 1.f })
			{
				AddPylon(FVector2D(GoalX, YSign * TowerY),
					TraceArenaConstants::GateTowerSide, TraceArenaConstants::GateTowerHeight,
					GateBodyMID, GateNeonMID, TEXT("GateTower"));
			}

			// The beam runs the FULL width, wall to wall, rather than only between the towers: its
			// ends bury themselves a few uu into the side walls, which is what makes it read as a
			// span carried by the arena instead of a bar balanced on two columns. It is 300 uu deep
			// and its top sits at 2600, flush with the wall tops.
			const float BeamZ = TraceArenaConstants::GateTowerHeight + TraceArenaConstants::GateBeamSize * 0.5f;
			UMaterialInstanceDynamic* GateFaceMID = MakeNeonMID(TeamColor, TraceArenaConstants::GlowFace);
			RegisterSideMID(Sign, GateFaceMID, /*bNeon=*/true, TraceArenaConstants::GlowFace);
			AddNeonBlock(FVector(GoalX, 0.f, BeamZ),
				FVector(TraceArenaConstants::GateBeamSize, ZoneWidth, TraceArenaConstants::GateBeamSize),
				0.f, GateBodyMID, GateNeonMID, /*bCollide=*/false, TEXT("GateBeam"),
				GateFaceMID, /*bVerticalTrim=*/false);   // a horizontal beam has no vertical corners worth lighting
		}

		// The trigger itself is server-only: scoring is an authority decision, and a client copy
		// would be dead weight at best. Spawned deferred so OwningTeam is correct before BeginPlay.
		if (!HasAuthority() || World == nullptr)
		{
			continue;
		}

		const FTransform ZoneTransform(
			GetActorRotation(),
			GetActorTransform().TransformPosition(FVector(CenterX, 0.f, WallHeight * 0.5f)));

		// Deferred so OwningTeam and the box extent are already correct when BeginPlay runs, and
		// RF_Transient because this is runtime scaffolding exactly like the player start pads: it
		// must never be serialised into a level, least of all by the editor preview below.
		FActorSpawnParameters ZoneParams;
		ZoneParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ZoneParams.ObjectFlags |= RF_Transient;
		ZoneParams.bDeferConstruction = true;

		ATraceEndzone* Zone = World->SpawnActor<ATraceEndzone>(
			ATraceEndzone::StaticClass(), ZoneTransform, ZoneParams);

		if (Zone == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("ATraceArenaBuilder: failed to spawn the %s endzone."), *TraceTeamName(Team).ToString());
			continue;
		}

		// Half extents: half the depth along X, the FULL half width along Y (sideline to sideline),
		// and floor to wall top in Z so a carrier scores whether they are running, jumping or
		// standing on anything built inside the zone.
		Zone->ConfigureZone(Team, FVector(Depth * 0.5f, HalfY, WallHeight * 0.5f), /*bInGoalVolume=*/false);

		// Armed state BEFORE BeginPlay, not after. ApplyScoringMode at the end of BuildArena would
		// reach the same answer, but the zone logs what it is on BeginPlay and a mode-B run would
		// otherwise print "Endzone ... is LIVE" a few lines before quietly disarming it. This project
		// has twice lost a day to a log line that said the wrong thing.
		Zone->SetZoneActive(!TraceIsGoalMode(ScoringMode));
		Zone->FinishSpawning(ZoneTransform);

		// Remembered so ApplyScoringMode can arm this pair and disarm the goals (or the reverse)
		// without a cast-and-filter walk of every actor in the world on every toggle.
		ScoringVolumes.Add(Zone);

		// Logged at Log, not Verbose, and with the numbers spelled out: "does the endzone really
		// span the whole field?" is otherwise a question you can only answer by walking into a
		// corner and hoping. X is the goal-line-to-end-wall span, Y is sideline to sideline.
		UE_LOG(LogTraceGame, Log,
			TEXT("Endzone (%s defends) spans X %.0f..%.0f, Y %.0f..%.0f (full field width %.0f), Z 0..%.0f."),
			*TraceTeamName(Team).ToString(),
			FMath::Min(GoalX, Sign * HalfX), FMath::Max(GoalX, Sign * HalfX),
			-HalfY, HalfY, ZoneWidth, WallHeight);

		// We are very often spawned from ATraceGameMode::BeginPlay, i.e. from inside the world's
		// own begin-play sweep, and an actor created during that sweep is not guaranteed to be
		// dispatched by it. The endzone does all of its wiring in BeginPlay, so force it here -
		// DispatchBeginPlay self-guards, and the world's later pass will simply skip it.
		//
		// NEVER during an editor preview: an editor world does not begin play at all, and starting a
		// scoring trigger ticking in the level editor is a side effect a preview button has no
		// business having. The box still draws its wireframe, which is all the preview needs it for.
		if (!bBuildingEditorPreview && !Zone->HasActorBegunPlay())
		{
			Zone->DispatchBeginPlay();
		}

		SpawnedActors.Add(Zone);
	}
}

void ATraceArenaBuilder::BuildGoalWall(float Sign, bool bBuildVisuals)
{
	// ---------------------------------------------------------------------------------------------
	// ONE END WALL, MODE B: the same slab BuildFloorAndWalls makes, with a circular hole through it.
	//
	// Four panels leave a square opening; RingSegments spokes close that square down to a circle;
	// a neon rim on the spokes' inner faces is the hoop you aim at; an alcove behind it gives the
	// hoop a room to frame instead of the empty sky; and four pawn shells replace the full-face one
	// that would otherwise hold a carrier 74 uu short of the mouth.
	//
	// Everything built here is collected into GoalModePieces by the caller, and the solid slab it
	// replaces is collected into EndzoneModePieces in BuildFloorAndWalls. Exactly one of the two is
	// presented, so mode A's end wall is untouched and cannot acquire a hole.
	// ---------------------------------------------------------------------------------------------

	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();
	const float Radius = GoalRingRadius();
	const float CentreZ = GoalRingCentreZ();

	// Half side of the square opening. Must clear the disc (>= Radius) and its corners must stay
	// inside the annulus (S * sqrt(2) <= outer radius), which is what the two scale constants buy.
	const float Square = Radius * TraceArenaConstants::GoalRingSquareScale;
	const float OuterRadius = Radius * TraceArenaConstants::GoalRingOuterScale;

	// The wall slab's own footprint, copied from BuildFloorAndWalls so the replacement occupies
	// exactly the volume the original did.
	const float WallCentreX = Sign * (HalfX + WallThickness * 0.5f);
	const float BottomZ = FMath::Max(0.f, CentreZ - Square);
	const float TopZ = FMath::Min(WallHeight, CentreZ + Square);

	// Which team defends THIS end, asked of the one function that answers it, so a half-time side
	// switch (which flips TeamEndSign's answer) cannot leave the hoop tinted for the wrong side.
	const ETraceTeam DefendingTeam = (TeamEndSign(ETraceTeam::Blue) == Sign) ? ETraceTeam::Blue : ETraceTeam::Orange;
	const FLinearColor TeamColor = TraceTeamColor(DefendingTeam);

	// The wall body keeps the neutral wall material; the hoop, the alcove and the rim take the colour
	// of the team that DEFENDS this end, exactly as the endzone patch does, and are registered for
	// the half-time repaint so the sides swap with everything else.
	UMaterialInstanceDynamic* WallMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::WallColor, 0.55f, 0.f, TraceArenaConstants::NeonNeutral, 0.022f)
		: nullptr;
	UMaterialInstanceDynamic* RingBodyMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.f, TeamColor, 0.035f)
		: nullptr;
	UMaterialInstanceDynamic* RingNeonMID = bBuildVisuals
		? MakeNeonMID(TeamColor, TraceArenaConstants::GlowGoalRing)
		: nullptr;
	// 0.20 -> 0.11 emissive after the first capture: at 0.20 the alcove was brighter than the hoop
	// around it and the goal photographed as a lit portal you look INTO rather than as a ring you
	// throw THROUGH. It has to be lit enough to say "there is a room back there" and dim enough that
	// the brightest thing at this end of the field is still the rim.
	UMaterialInstanceDynamic* AlcoveMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::Dim(TeamColor, 0.020f), 0.35f, 0.f, TeamColor, 0.11f)
		: nullptr;

	RegisterSideMID(Sign, RingBodyMID, /*bNeon=*/false, /*Intensity=*/0.035f, /*BaseDim=*/-1.f);
	RegisterSideMID(Sign, RingNeonMID, /*bNeon=*/true, TraceArenaConstants::GlowGoalRing);
	RegisterSideMID(Sign, AlcoveMID, /*bNeon=*/false, /*Intensity=*/0.11f, /*BaseDim=*/0.020f);

	// --- 1. The four panels around the square opening --------------------------------------------
	//
	// Full field width below and above, and the two side columns between them. Built through
	// AddCollisionBlock + AddMeshBlock rather than AddNeonBlock: this is WALL, and a lip and face trim
	// on each of four fragments would draw four bright rectangles around a hole that already has the
	// brightest object in the arena around it.
	struct FPanel
	{
		FVector Centre;
		FVector Size;
	};

	const FPanel Panels[] =
	{
		// Below the opening, and above it: the full width of the wall.
		{ FVector(WallCentreX, 0.f, BottomZ * 0.5f),
		  FVector(WallThickness, FieldWidth + 2.f * WallThickness, FMath::Max(1.f, BottomZ)) },
		{ FVector(WallCentreX, 0.f, (TopZ + WallHeight) * 0.5f),
		  FVector(WallThickness, FieldWidth + 2.f * WallThickness, FMath::Max(1.f, WallHeight - TopZ)) },
		// The two columns either side of the opening, spanning only its height.
		{ FVector(WallCentreX, (Square + HalfY + WallThickness) * 0.5f, CentreZ),
		  FVector(WallThickness, FMath::Max(1.f, HalfY + WallThickness - Square), TopZ - BottomZ) },
		{ FVector(WallCentreX, -(Square + HalfY + WallThickness) * 0.5f, CentreZ),
		  FVector(WallThickness, FMath::Max(1.f, HalfY + WallThickness - Square), TopZ - BottomZ) }
	};

	for (const FPanel& Panel : Panels)
	{
		AddCollisionBlock(Panel.Centre, Panel.Size, TEXT("GoalWallPanel"));
		if (bBuildVisuals)
		{
			AddMeshBlock(CubeMesh, Panel.Centre, Panel.Size, WallMID, /*bCastShadow=*/true, TEXT("GoalWallPanel"));
		}
	}

	// --- 2. The annulus: square opening -> circular hole ------------------------------------------
	//
	// Each spoke is a box rolled about the field axis, with its LOCAL Y radial and its LOCAL Z
	// tangential. Its inner face is therefore a chord at exactly Radius from the centre, so the union
	// of the inner faces is a regular polygon circumscribing the scoring disc - the physical hole is
	// never smaller than the disc that scores. Tangential width is sized at the OUTER radius (where
	// the sectors are widest) and overlapped, so there is no seam to squeeze a Core through.
	const int32 Segments = TraceArenaConstants::GoalRingSegments;
	const float SectorHalfAngle = PI / static_cast<float>(Segments);
	const float RadialDepth = FMath::Max(40.f, OuterRadius - Radius);
	const float MidRadius = (Radius + OuterRadius) * 0.5f;
	const float SpokeWidth = 2.f * OuterRadius * FMath::Tan(SectorHalfAngle) * TraceArenaConstants::GoalRingSpokeOverlap;

	for (int32 Index = 0; Index < Segments; ++Index)
	{
		const float Angle = (2.f * PI * static_cast<float>(Index)) / static_cast<float>(Segments);
		const float CosA = FMath::Cos(Angle);
		const float SinA = FMath::Sin(Angle);

		// Roll about local X. Roll takes local +Y to (0, cos, sin), which is the radial direction, and
		// local +Z to the tangent - so Size is (through the wall, radial, tangential).
		const FRotator SpokeRotation(0.f, 0.f, FMath::RadiansToDegrees(Angle));
		const FVector SpokeCentre(WallCentreX, MidRadius * CosA, CentreZ + MidRadius * SinA);
		const FVector SpokeSize(WallThickness, RadialDepth, SpokeWidth);

		AddCollisionBlockRotated(SpokeCentre, SpokeSize, TEXT("GoalRingSpoke"), SpokeRotation);

		if (!bBuildVisuals)
		{
			continue;
		}

		AddMeshBlockRotated(CubeMesh, SpokeCentre, SpokeSize, RingBodyMID, /*bCastShadow=*/true,
			TEXT("GoalRingSpoke"), SpokeRotation);

		// THE HOOP. A thin bright band on the spoke's inner face, standing proud of the wall on the
		// field side so it is visible edge-on from the halfway line as well as head on. This is the
		// entire answer to "make it visually obvious what and where the goal is": a ring set into a
		// near-black wall is invisible without it.
		const float RimRadius = Radius + TraceArenaConstants::GoalRingRimDepth * 0.5f;

		// SIZED AT THE RIM'S OWN RADIUS, not at the spoke's. MEASURED: reusing SpokeWidth here (which
		// is sized for the annulus's OUTER radius, where the sectors are widest) made each rim bar 690
		// uu long on a circle that only needs 410 at that radius - so they overshot each other by 68%
		// and the hoop photographed as a jagged pinwheel of crossing bars rather than as a ring. The
		// 1.06 is the seam overlap and nothing more.
		const float RimWidth = 2.f * RimRadius * FMath::Tan(SectorHalfAngle) * 1.06f;

		const FVector RimCentre(
			WallCentreX - Sign * (WallThickness * 0.5f + TraceArenaConstants::GoalRingRimProud * 0.5f),
			RimRadius * CosA, CentreZ + RimRadius * SinA);
		const FVector RimSize(
			WallThickness + TraceArenaConstants::GoalRingRimProud,
			TraceArenaConstants::GoalRingRimDepth,
			RimWidth);

		AddMeshBlockRotated(CubeMesh, RimCentre, RimSize, RingNeonMID, /*bCastShadow=*/false,
			TEXT("GoalRingRim"), SpokeRotation);
	}

	// --- 3. The alcove behind the hoop ------------------------------------------------------------
	//
	// A short lit tube and a back panel. Without it the hoop frames the void outside the arena, which
	// reads as a hole in the level rather than as a goal; with it the mouth glows from the inside and
	// a Core (or a carrier) that goes through lands somewhere real instead of falling out of the
	// world. It sits OUTSIDE the field box, so nothing on the pitch can reach it except through the
	// ring - and by then the goal has already been awarded.
	const float AlcoveNearX = Sign * (HalfX + WallThickness);
	const float AlcoveDepth = TraceArenaConstants::GoalAlcoveDepth;
	const float AlcoveCentreX = AlcoveNearX + Sign * AlcoveDepth * 0.5f;
	const float AlcoveHalf = Square;
	const float AlcoveWall = 60.f;

	const FPanel Alcove[] =
	{
		{ FVector(AlcoveCentreX, 0.f, CentreZ - AlcoveHalf - AlcoveWall * 0.5f),
		  FVector(AlcoveDepth, AlcoveHalf * 2.f + AlcoveWall * 2.f, AlcoveWall) },
		{ FVector(AlcoveCentreX, 0.f, CentreZ + AlcoveHalf + AlcoveWall * 0.5f),
		  FVector(AlcoveDepth, AlcoveHalf * 2.f + AlcoveWall * 2.f, AlcoveWall) },
		{ FVector(AlcoveCentreX, AlcoveHalf + AlcoveWall * 0.5f, CentreZ),
		  FVector(AlcoveDepth, AlcoveWall, AlcoveHalf * 2.f) },
		{ FVector(AlcoveCentreX, -(AlcoveHalf + AlcoveWall * 0.5f), CentreZ),
		  FVector(AlcoveDepth, AlcoveWall, AlcoveHalf * 2.f) },
		{ FVector(AlcoveNearX + Sign * (AlcoveDepth + AlcoveWall * 0.5f), 0.f, CentreZ),
		  FVector(AlcoveWall, AlcoveHalf * 2.f + AlcoveWall * 2.f, AlcoveHalf * 2.f + AlcoveWall * 2.f) }
	};

	for (const FPanel& Piece : Alcove)
	{
		AddCollisionBlock(Piece.Centre, Piece.Size, TEXT("GoalAlcove"));
		if (bBuildVisuals)
		{
			AddMeshBlock(CubeMesh, Piece.Centre, Piece.Size, AlcoveMID, /*bCastShadow=*/false, TEXT("GoalAlcove"));
		}
	}

	// --- 4. Pawn standoffs, everywhere except the mouth -------------------------------------------
	//
	// The same WallNeonStandoff shell BuildFloorAndWalls runs across the whole end wall, cut into
	// four pieces around the opening. Pawn-only, like every other standoff: shots, sightlines, the
	// Core and the camera probe still see the real wall.
	const float StandoffX = Sign * (HalfX - TraceArenaConstants::WallNeonStandoff * 0.5f);
	const FPanel Shells[] =
	{
		{ FVector(StandoffX, 0.f, BottomZ * 0.5f),
		  FVector(TraceArenaConstants::WallNeonStandoff, FieldWidth, FMath::Max(1.f, BottomZ)) },
		{ FVector(StandoffX, 0.f, (TopZ + WallHeight) * 0.5f),
		  FVector(TraceArenaConstants::WallNeonStandoff, FieldWidth, FMath::Max(1.f, WallHeight - TopZ)) },
		{ FVector(StandoffX, (Square + HalfY) * 0.5f, CentreZ),
		  FVector(TraceArenaConstants::WallNeonStandoff, FMath::Max(1.f, HalfY - Square), TopZ - BottomZ) },
		{ FVector(StandoffX, -(Square + HalfY) * 0.5f, CentreZ),
		  FVector(TraceArenaConstants::WallNeonStandoff, FMath::Max(1.f, HalfY - Square), TopZ - BottomZ) }
	};

	for (const FPanel& Shell : Shells)
	{
		AddPawnStandoff(Shell.Centre, Shell.Size, TEXT("GoalWallStandoff"));
	}

	// --- 5. The approach ramp ---------------------------------------------------------------------
	//
	// WITHOUT THIS THERE IS NO CARRY-IN GOAL, and that is worth stating plainly because it is a
	// gameplay consequence of a purely visual-sounding instruction. Raising the mouth 1.5 player
	// heights puts the bottom of the hoop at 264 uu; a pawn's origin sits at 88 uu standing and
	// reaches roughly 208 uu at the top of a jump, so a carrier on the flat can never get their
	// origin inside the disc and "scoring by carrying through it" would be dead on arrival. A shallow
	// slope (13 degrees at the default, well inside the walkable angle) up to one step BELOW the hoop
	// puts a standing carrier's origin 952 uu from the ring centre - inside a 1000 uu mouth.
	//
	// It also gives the defence something: the ramp is the only way in on foot, it is 2000 uu wide,
	// and it is the obvious place to hold.
	const float RampTop = GoalRampTopZ();
	if (RampTop > 1.f)
	{
		const float Run = FMath::Max(200.f, RampTop * FMath::Max(0.5f, GoalRampRunPerRise));
		const float SlopeLength = FMath::Sqrt(Run * Run + RampTop * RampTop);
		const float SlopeRadians = FMath::Atan2(RampTop, Run);
		const float SlopeDegrees = FMath::RadiansToDegrees(SlopeRadians);

		// Positive pitch tilts the block's local +X upward, so the ramp rises toward the wall at the
		// +X end and away from it at the -X end: Pitch = Sign * slope.
		const FRotator RampRotation(Sign * SlopeDegrees, 0.f, 0.f);

		// Centre of the sloped slab, dropped half a thickness along its own normal so the TOP surface
		// is the line from (wall - Run, 0) to (wall, RampTop) rather than the middle of the slab.
		const FVector RampCentre(
			Sign * (HalfX - Run * 0.5f) + Sign * FMath::Sin(SlopeRadians) * TraceArenaConstants::GoalRampThickness * 0.5f,
			0.f,
			RampTop * 0.5f - FMath::Cos(SlopeRadians) * TraceArenaConstants::GoalRampThickness * 0.5f);

		const FVector RampSize(SlopeLength, Radius * 2.f, TraceArenaConstants::GoalRampThickness);

		AddCollisionBlockRotated(RampCentre, RampSize, TEXT("GoalRamp"), RampRotation);

		if (bBuildVisuals)
		{
			UMaterialInstanceDynamic* RampMID = MakeSurfaceMID(
				TraceArenaConstants::Dim(TeamColor, 0.035f), 0.30f, 0.f, TeamColor, 0.10f);
			RegisterSideMID(Sign, RampMID, /*bNeon=*/false, /*Intensity=*/0.10f, /*BaseDim=*/0.035f);

			AddMeshBlockRotated(CubeMesh, RampCentre, RampSize, RampMID, /*bCastShadow=*/false,
				TEXT("GoalRamp"), RampRotation);

			// Two lit edges down the sides of the ramp, so its width reads from the halfway line.
			for (const float YSign : { -1.f, 1.f })
			{
				const FVector EdgeCentre(RampCentre.X, YSign * Radius, RampCentre.Z + TraceArenaConstants::GoalRampThickness * 0.5f);
				AddMeshBlockRotated(CubeMesh, EdgeCentre,
					FVector(SlopeLength, TraceArenaConstants::GoalSillWidth * 0.7f, TraceArenaConstants::GoalLineThickness),
					RingNeonMID, /*bCastShadow=*/false, TEXT("GoalRampEdge"), RampRotation);
			}
		}
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("Goal wall (%s end): opening +/-%.0f uu square at Z %.0f, hoop radius %.0f (%.0f uu across), ")
		TEXT("bottom of the hoop %.0f uu up, %d spokes, ramp to %.0f uu."),
		*TraceTeamName(DefendingTeam).ToString(), Square, CentreZ, Radius, Radius * 2.f,
		CentreZ - Radius, Segments, RampTop);
}

void ATraceArenaBuilder::BuildGoals(bool bBuildVisuals)
{
	// ---------------------------------------------------------------------------------------------
	// MODE B - the goals. Spec v6 section 4.3, verbatim: "Raise the goals 1.5x player height from the
	// ground, place them into the back walls, and make them circular."
	//
	// WHAT THIS REPLACED. Until v6 a goal was furniture standing on the floor between the goal line
	// and the end wall - two posts, a crossbar, a lit sill and a tinted mouth patch - and the scoring
	// volume was the endzone-deep box behind it. All of that is gone. The goal is now a HOLE THROUGH
	// THE BACK WALL with a neon hoop around it (BuildGoalWall), and the scoring volume is a shallow
	// slab across its mouth which scores on the DISC inscribed in it, not on the slab's corners.
	//
	// The floor dressing that survives is the approach lane: the goal line sill and the two rails
	// running back from it still mark the 2000 uu corridor that leads to the ramp, which is what tells
	// a carrier at midfield which lane the goal is in. Everything here is tagged into GoalModePieces
	// and hidden (and de-collided) while mode A is being played.
	// ---------------------------------------------------------------------------------------------

	UWorld* World = GetWorld();
	const float HalfX = HalfLength();
	const float MouthHalfY = GoalHalfWidth();
	const float Depth = ClampedEndzoneDepth();
	const float Radius = GoalRingRadius();
	const float CentreZ = GoalRingCentreZ();
	const float SlabDepth = FMath::Clamp(GoalRingDepth, 60.f, Depth);

	// Mark BEFORE anything is built, collision included: the perforated wall BLOCKS, and on a
	// dedicated server (which builds collision and no visuals at all) an untagged panel would be a
	// second wall standing inside the first one through the whole of mode A.
	const int32 GoalMark = MarkBuiltComponents();

	// Say the finished shape out loud, in uu, at Display. This is the line a playtest of spec v6 is
	// judged against, and none of it - the height off the floor, the diameter, the fact that it is a
	// circle in a wall rather than a box on the grass - is readable from a screenshot. It also states
	// the resolution of the spec's own internal contradiction (see the header), so nobody has to
	// re-derive it from the numbers.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Arena] MODE B goal (spec v6): CIRCULAR, %.0f uu across, set into the back wall, ")
		TEXT("centre Z %.0f, bottom of the hoop %.0f uu off the floor (= %.2f x the %.0f uu player height), ")
		TEXT("scoring slab %.0f uu deep."),
		Radius * 2.f, CentreZ, CentreZ - Radius,
		(PlayerHeightUU() > 0.f) ? ((CentreZ - Radius) / PlayerHeightUU()) : 0.f, PlayerHeightUU(),
		SlabDepth);

	const ETraceTeam Teams[] = { ETraceTeam::Blue, ETraceTeam::Orange };
	for (const ETraceTeam Team : Teams)
	{
		const float Sign = TeamEndSign(Team);
		const float GoalX = Sign * GoalLineX();
		const float CenterX = Sign * (HalfX - Depth * 0.5f);
		const FLinearColor TeamColor = TraceTeamColor(Team);

		// The wall, the hoop, the alcove and the ramp.
		BuildGoalWall(Sign, bBuildVisuals);

		if (!bBuildVisuals)
		{
			continue;
		}

		UMaterialInstanceDynamic* FrameMID = MakeNeonMID(TeamColor, TraceArenaConstants::GlowGoalFrame);
		RegisterSideMID(Sign, FrameMID, /*bNeon=*/true, TraceArenaConstants::GlowGoalFrame);

		// --- The approach lane on the floor -------------------------------------------------------
		//
		// A goal 264 uu up a wall is not visible on the FLOOR, and the floor is where a carrier's eye
		// spends the run-in. The sill on the goal line and the two rails running back from it draw the
		// 2000 uu corridor that ends at the ramp, so "the goal is down this lane" is legible from
		// ground level at midfield - the same job the mouth patch did before v6.
		AddMeshBlock(CubeMesh,
			FVector(GoalX, 0.f, TraceArenaConstants::GoalLineZ + 4.f),
			FVector(TraceArenaConstants::GoalSillWidth, MouthHalfY * 2.f, TraceArenaConstants::GoalLineThickness),
			FrameMID, /*bCastShadow=*/false, TEXT("GoalSill"));

		UMaterialInstanceDynamic* MouthMID = MakeSurfaceMID(
			TraceArenaConstants::Dim(TeamColor, 0.030f), 0.20f, 0.f, TeamColor, 0.16f);
		RegisterSideMID(Sign, MouthMID, /*bNeon=*/false, /*Intensity=*/0.16f, /*BaseDim=*/0.030f);
		AddMeshBlock(CubeMesh,
			FVector(CenterX, 0.f, TraceArenaConstants::GoalPatchZ),
			FVector(Depth, MouthHalfY * 2.f, TraceArenaConstants::PatchThickness),
			MouthMID, /*bCastShadow=*/false, TEXT("GoalMouthPatch"));

		for (const float YSign : { -1.f, 1.f })
		{
			AddMeshBlock(CubeMesh,
				FVector(CenterX, YSign * MouthHalfY, TraceArenaConstants::GoalLineZ + 4.f),
				FVector(Depth, TraceArenaConstants::GoalSillWidth * 0.6f, TraceArenaConstants::GoalLineThickness),
				FrameMID, /*bCastShadow=*/false, TEXT("GoalSideRail"));
		}
	}

	// Everything above belongs to mode B. Collected once, outside the team loop, because the tag is
	// about the MODE and not about the end.
	CollectPiecesSince(GoalMark, GoalModePieces);

	// --- Triggers ---------------------------------------------------------------------------------
	//
	// Server only, exactly like the endzone triggers, and spawned deferred for the same reason.
	if (!HasAuthority() || World == nullptr)
	{
		return;
	}

	for (const ETraceTeam Team : Teams)
	{
		const float Sign = TeamEndSign(Team);

		// CENTRED ON THE HOOP, not on the endzone footprint: the volume is the ring's bounding slab,
		// sitting just inside the wall plane, and ConfigureRing turns it into a disc. That is the whole
		// of "the goal is circular and it is in the wall" as far as the scoring rule is concerned.
		const FTransform ZoneTransform(
			GetActorRotation(),
			GetActorTransform().TransformPosition(FVector(Sign * (HalfX - SlabDepth * 0.5f), 0.f, CentreZ)));

		FActorSpawnParameters ZoneParams;
		ZoneParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ZoneParams.ObjectFlags |= RF_Transient;
		ZoneParams.bDeferConstruction = true;

		ATraceEndzone* Goal = World->SpawnActor<ATraceEndzone>(
			ATraceEndzone::StaticClass(), ZoneTransform, ZoneParams);

		if (Goal == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("ATraceArenaBuilder: failed to spawn the %s goal."), *TraceTeamName(Team).ToString());
			continue;
		}

		Goal->ConfigureZone(Team, FVector(SlabDepth * 0.5f, Radius, Radius), /*bInGoalVolume=*/true);
		Goal->ConfigureRing(Radius);
		Goal->SetZoneActive(TraceIsGoalMode(ScoringMode));   // see the matching note in BuildEndzones
		Goal->FinishSpawning(ZoneTransform);
		ScoringVolumes.Add(Goal);

		UE_LOG(LogTraceGame, Log,
			TEXT("Goal (%s defends) is a RING of radius %.0f uu centred at X %.0f, Y 0, Z %.0f, ")
			TEXT("slab X %.0f..%.0f (%.0f uu across = %.0f%% of the %.0f uu field width)."),
			*TraceTeamName(Team).ToString(), Radius,
			Sign * (HalfX - SlabDepth * 0.5f), CentreZ,
			FMath::Min(Sign * (HalfX - SlabDepth), Sign * HalfX),
			FMath::Max(Sign * (HalfX - SlabDepth), Sign * HalfX),
			Radius * 2.f, (FieldWidth > 0.f) ? (Radius * 200.f / FieldWidth) : 0.f, FieldWidth);

		if (!bBuildingEditorPreview && !Goal->HasActorBegunPlay())
		{
			Goal->DispatchBeginPlay();
		}

		SpawnedActors.Add(Goal);
	}
}

void ATraceArenaBuilder::BuildPlayerStarts()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const int32 PerTeam = FMath::Clamp(StartsPerTeam, 1, 16);
	const float HalfX = HalfLength();
	const float Spread = HalfWidth() * TraceArenaConstants::StartSpreadFraction;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient; // Purely runtime scaffolding; never save it into a level.

	const ETraceTeam Teams[] = { ETraceTeam::Blue, ETraceTeam::Orange };
	for (const ETraceTeam Team : Teams)
	{
		const float Sign = TeamEndSign(Team);

		// A team spawns in front of the endzone it defends and faces the centre of the field.
		//
		// Depth comes from the SAME function BuildEndzones uses (ClampedEndzoneDepth), not from a
		// second hand-written clamp, so the pads cannot end up inside (or on the far side of) the
		// endzone they are supposed to sit in front of. Widening the endzone to the full field width
		// changed nothing here - the line is a function of depth alone - but the lateral spread is
		// worth a thought: it is StartSpreadFraction (0.6) of the half width, i.e. +/-2880 on the
		// 9600 uu field, which clears the corner banks by 420 uu. The lower bound is the centre
		// diamond rather than 0: with a large EndzoneDepth the old Max(0.f, ...) put BOTH teams'
		// spawn lines at X = 0, i.e. on top of it — silently reproducing the exact "spawned inside
		// geometry" failure this build was shipped with.
		//
		// THE RESULTING FAN, on the 33600 x 9600 field: X = +/-12720 (i.e. 1680 uu in front of the
		// goal line at 14400), Y = 0 / +/-1440 / +/-2880. Every one of those five points was
		// re-checked against every entry in ApproachCover and MidfieldCover and against the corner
		// banks. The tightest is the 3.5x goal-approach tower E at (13800, 2100), whose 45-degree
		// footprint reaches X = 13305 — 585 uu clear in X — and whose Y span deliberately falls
		// BETWEEN two pad rows; then block D at (11000, 1300), 890 uu clear.
		//
		// THIS IS THE PAIRING THAT BROKE WHEN THE FIELD WAS LENGTHENED, and it is worth knowing why:
		// the pad line is GOAL-RELATIVE (goal line minus 0.1 * half length) while the cover used to
		// be LENGTH-RELATIVE, so growing the field slid them through each other and put a pad inside
		// block E's footprint. ApproachCover is now measured back from the goal line for exactly
		// that reason. If you move the interior layout, re-check it: a start pad inside a solid block
		// is a pawn that spawns embedded in the world, and nothing logs it.
		const float Depth = ClampedEndzoneDepth();
		const float MinLineX = TraceArenaConstants::DaisTopTierSide + TraceArenaConstants::DaisTierSideStep * TraceArenaConstants::DaisTiers;
		const float LineX = Sign * FMath::Max(MinLineX, HalfX * (1.f - TraceArenaConstants::StartInsetFraction) - Depth);
		const float FacingYaw = (Sign < 0.f) ? 0.f : 180.f;

		for (int32 Index = 0; Index < PerTeam; ++Index)
		{
			const float Alpha = (PerTeam == 1) ? 0.5f : static_cast<float>(Index) / static_cast<float>(PerTeam - 1);
			const float Y = FMath::Lerp(-Spread, Spread, Alpha);

			const FVector Location = GetActorTransform().TransformPosition(
				FVector(LineX, Y, TraceArenaConstants::PlayerStartZ));
			const FRotator Rotation = GetActorRotation() + FRotator(0.f, FacingYaw, 0.f);

			ATraceTeamPlayerStart* Start = World->SpawnActor<ATraceTeamPlayerStart>(
				ATraceTeamPlayerStart::StaticClass(), Location, Rotation, SpawnParams);

			if (Start == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("ATraceArenaBuilder: failed to spawn a %s player start."), *TraceTeamName(Team).ToString());
				continue;
			}

			Start->Team = Team;
			SpawnedActors.Add(Start);
		}
	}

	UE_LOG(LogTraceGame, Verbose, TEXT("Arena spawned %d player starts per team."), PerTeam);
}

void ATraceArenaBuilder::BuildLighting()
{
	UWorld* World = GetWorld();
	if (World == nullptr || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// Every actor here is a local cosmetic, spawned independently on the server and on every client,
	// so each machine must be told explicitly NOT to replicate its copy. ALight (and ASkyLight) turn
	// replication and bAlwaysRelevant on in their own constructors, which on a listen server would
	// push the host's lights to every client ON TOP of the ones that client just built for itself:
	// double intensity everywhere, with r.DefaultFeature.AutoExposure=False and no auto-exposure to
	// hide behind. Gating the spawn on net mode instead would be wrong - clients genuinely need
	// their own lights.
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	const FVector SkyPosition = GetActorTransform().TransformPosition(FVector(0.f, 0.f, WallHeight * 2.f));

	auto MakeLocal = [](AActor* Actor)
	{
		if (Actor != nullptr)
		{
			Actor->SetReplicates(false);
			Actor->SetReplicateMovement(false);
		}
		return Actor;
	};

	// --- Atmosphere ------------------------------------------------------------------------------
	//
	// Kept, but driven to NIGHT. This is the delicate part of the whole file, so the reasoning in
	// full:
	//
	//  * Removing it entirely is tempting - Tron has a black sky - but it is also how this project
	//    shipped a black screen once. With no atmosphere the sky light's real-time capture sees
	//    nothing (SLS_CapturedScene only includes geometry beyond SkyDistanceThreshold = 150000 uu,
	//    and the arena is 24000), so ambient becomes exactly zero and every surface facing away from
	//    the key light renders at literally (0,0,0). Emissive materials cannot rescue that: global
	//    illumination is off (r.DynamicGlobalIlluminationMethod=0), so a glowing strip lights
	//    nothing but itself.
	//  * Keeping it and pointing the sun DOWN is how the previous build got a bright blue daytime
	//    sky over a light grey floor - the exact opposite of the art direction.
	//
	// So: the atmosphere stays, and its sun sits just BELOW the horizon (positive pitch = the light
	// travels upward). The sky renders as deep twilight - near black overhead with a faint horizon
	// band - the real-time sky light captures that and turns it into a small amount of cool ambient,
	// and because the light is below the horizon it puts essentially nothing on the upward-facing
	// floor. Dark world, non-zero ambient, no black screen.
	//
	// Defaults do the rest: TransformMode is PlanetTopAtAbsoluteWorldOrigin, which puts the virtual
	// ground at Z = 0 - exactly where the arena floor top is. ASkyAtmosphere is an AInfo with no
	// collision and no gameplay presence.
	if (AActor* Atmosphere = MakeLocal(World->SpawnActor<ASkyAtmosphere>(ASkyAtmosphere::StaticClass(), FTransform(SkyPosition), SpawnParams)))
	{
		SpawnedActors.Add(Atmosphere);
	}

	// --- Directional lights ----------------------------------------------------------------------

	struct FLightSpec
	{
		FRotator Rotation;
		float Intensity;
		FLinearColor Color;
		bool bCastShadows;
		bool bAtmosphereSun;
		const TCHAR* Name;
	};

	const FLightSpec Lights[] =
	{
		// The atmosphere's sun. Dim and below the horizon; it is here for the sky, not for the arena.
		{ AtmosphereSunRotation, AtmosphereSunIntensity, FLinearColor(0.55f, 0.68f, 1.00f), false, true, TEXT("AtmosphereSun") },
		// Key. The only shadow caster in the scene - one shadow pass, one shadow per character.
		{ KeyLightRotation, KeyLightIntensity, FLinearColor(0.62f, 0.78f, 1.00f), true, false, TEXT("KeyLight") },
		// Warm rim from the opposite azimuth. Two of the four wall inner faces have a negative N.L
		// against the key and would otherwise be lit by ambient alone; this also gives characters a
		// warm edge that separates them from a cyan background.
		{ FillLightRotation, FillLightIntensity, FLinearColor(1.00f, 0.46f, 0.22f), false, false, TEXT("FillLight") },
		// Faint cyan travelling upward - the bounce a floor covered in glowing lines would give if
		// this project had global illumination. Without it every underside is pure black.
		{ BounceLightRotation, BounceLightIntensity, FLinearColor(0.20f, 0.62f, 0.95f), false, false, TEXT("BounceLight") }
	};

	for (const FLightSpec& Spec : Lights)
	{
		if (Spec.Intensity <= 0.f)
		{
			continue;
		}

		const FTransform Transform(GetActorRotation() + Spec.Rotation, SkyPosition);
		ADirectionalLight* Light = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), Transform, SpawnParams);
		if (Light == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("ATraceArenaBuilder: failed to spawn the %s directional light."), Spec.Name);
			continue;
		}
		MakeLocal(Light);

		// FindComponentByClass rather than a typed accessor: it cannot be wrong about the getter's
		// name or return type on any 5.x engine.
		if (UDirectionalLightComponent* LightComponent = Light->FindComponentByClass<UDirectionalLightComponent>())
		{
			// Runtime-spawned lights must be Movable. A Stationary/Static light has no baked data in
			// a level nobody ever built lighting for, and the project disables static lighting
			// entirely (r.AllowStaticLighting=False), so Movable is the only correct answer.
			LightComponent->SetMobility(EComponentMobility::Movable);
			LightComponent->SetIntensity(Spec.Intensity);
			LightComponent->SetLightColor(Spec.Color);
			LightComponent->SetCastShadows(Spec.bCastShadows);
			LightComponent->SetAtmosphereSunLight(Spec.bAtmosphereSun);
		}

		SpawnedActors.Add(Light);
	}

	// --- Sky light -------------------------------------------------------------------------------

	if (ASkyLight* Sky = World->SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FTransform(SkyPosition), SpawnParams))
	{
		MakeLocal(Sky);

		if (USkyLightComponent* SkyComponent = Sky->FindComponentByClass<USkyLightComponent>())
		{
			SkyComponent->SetMobility(EComponentMobility::Movable);

			// Intensity is set through the public field plus MarkRenderStateDirty rather than a
			// setter: ULightComponentBase::Intensity is stable across the whole 5.x line, whereas the
			// sky light's typed SetIntensity lives on USkyLightComponent rather than ULightComponent
			// and we cannot compile-check it here.
			SkyComponent->Intensity = SkyLightIntensity;
			SkyComponent->MarkRenderStateDirty();

			// Real-time capture, not the default one-shot capture. The default only includes geometry
			// beyond SkyDistanceThreshold (150000 uu) - i.e. nothing inside a 24000 uu arena - so it
			// would capture black and this light would contribute exactly nothing. Real-time capture
			// sees the SkyAtmosphere spawned above and turns its twilight into genuine ambient, which
			// is the only thing stopping shadowed surfaces rendering at (0,0,0).
			SkyComponent->SetRealTimeCapture(true);

			// The lower hemisphere is forced to black by default, which would throw away the bounce
			// off a floor that is covered in glowing lines. A dim tint of the neon palette instead.
			SkyComponent->bLowerHemisphereIsBlack = false;
			SkyComponent->SetLowerHemisphereColor(FLinearColor(0.010f, 0.030f, 0.045f));

			// Harmless with real-time capture on (which recaptures every frame anyway), but it is the
			// documented way to prime a runtime-spawned sky light and costs one frame.
			SkyComponent->RecaptureSky();
		}
		SpawnedActors.Add(Sky);
	}

	// --- Height fog ------------------------------------------------------------------------------
	//
	// Cheap depth cueing, and on a field this long it is doing real work: without it the far endzone
	// gate and the near cover blocks are the same brightness and the arena reads as a flat backdrop.
	// The colour is a dark blue that the neon reads against; density is low enough that the far wall
	// is still legible.
	if (AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(AExponentialHeightFog::StaticClass(), FTransform(GetActorLocation()), SpawnParams))
	{
		MakeLocal(Fog);

		if (UExponentialHeightFogComponent* FogComponent = Fog->FindComponentByClass<UExponentialHeightFogComponent>())
		{
			FogComponent->SetMobility(EComponentMobility::Movable);
			FogComponent->SetFogDensity(FogDensity);
			FogComponent->SetFogInscatteringColor(FLinearColor(0.015f, 0.045f, 0.075f));
			FogComponent->SetFogHeightFalloff(0.08f);
			FogComponent->SetStartDistance(800.f);
		}
		SpawnedActors.Add(Fog);
	}
}

void ATraceArenaBuilder::BuildPostProcess()
{
	UWorld* World = GetWorld();
	if (World == nullptr || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(
		APostProcessVolume::StaticClass(), FTransform(GetActorLocation()), SpawnParams);
	if (Volume == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("ATraceArenaBuilder: could not spawn the post-process volume; bloom will be at engine defaults."));
		return;
	}

	Volume->SetReplicates(false);
	Volume->SetReplicateMovement(false);

	// Unbound: the volume applies everywhere regardless of its (empty, runtime-spawned) brush, which
	// is the only sane option for a volume nobody authored a shape for.
	Volume->bUnbound = true;
	Volume->bEnabled = true;
	Volume->BlendWeight = 1.f;
	Volume->Priority = 1.f;

	FPostProcessSettings& PP = Volume->Settings;

	// BLOOM IS THE WHOLE POINT. An unlit emissive strip without bloom is a flat coloured rectangle;
	// with bloom it is a glowing tube, and that difference is the difference between "grey box with
	// stripes" and "Tron". A negative threshold means "bloom everything, weighted by brightness"
	// instead of only pixels over 1.0, so the dimmer floor grid glows too and not just the hot trim.
	PP.bOverride_BloomIntensity = true;
	PP.BloomIntensity = BloomIntensity;
	PP.bOverride_BloomThreshold = true;
	PP.BloomThreshold = BloomThreshold;

	// Bloom SIZE, as distinct from bloom intensity, and the difference matters for "refined".
	// Intensity is how much energy the glow carries; size is how far it is smeared. The engine
	// default of 4 spreads a neon strip over a very wide kernel, which is what makes a frame full of
	// thin bright lines read as soft rather than sharp - the same complaint that cost this project
	// most of its chromatic aberration and half a point of bloom intensity. 3.0 keeps the tube
	// (the glow is still obviously a glow, not a coloured rectangle) while tightening it around the
	// line that produced it, so edges stay edges. This is the cheapest available sharpness win that
	// does not touch the art direction: no intensity was removed, only spread.
	PP.bOverride_BloomSizeScale = true;
	PP.BloomSizeScale = 3.0f;

	// Screen-space reflections, cranked. The floor is a near-black low-roughness surface and the
	// reflection of the neon in it is most of what you actually see on the ground.
	//
	// Quality 80 -> 100. SSR is stochastic and its noise floor is set by the ray count, which is
	// what Quality buys; on a NEAR-BLACK floor that noise is the only thing in the lower third of
	// most frames, so it is unusually visible here - the DefaultEngine.ini comment about SSR grain
	// is about exactly this surface. 100 is the top step and it is cheap on a scene with 800-odd
	// simple opaque draws and no Nanite, no Lumen and no virtual shadow maps.
	PP.bOverride_ScreenSpaceReflectionIntensity = true;
	PP.ScreenSpaceReflectionIntensity = 70.f;
	PP.bOverride_ScreenSpaceReflectionQuality = true;
	PP.ScreenSpaceReflectionQuality = 100.f;
	PP.bOverride_ScreenSpaceReflectionMaxRoughness = true;
	PP.ScreenSpaceReflectionMaxRoughness = 0.55f;

	// Tonemapper: a steeper slope and a deeper toe crush the near-black structure toward true black
	// while leaving the neon where it is, which is what gives the image its contrast. Without this
	// the dark albedos come back as muddy grey.
	PP.bOverride_FilmSlope = true;
	PP.FilmSlope = 0.92f;
	PP.bOverride_FilmToe = true;
	PP.FilmToe = 0.85f;
	PP.bOverride_FilmShoulder = true;
	PP.FilmShoulder = 0.30f;
	PP.bOverride_FilmBlackClip = true;
	PP.FilmBlackClip = 0.f;

	// A little extra saturation so the two team colours stay unmistakably apart at distance, and a
	// vignette to pull the eye off the huge dark walls and into the middle of the frame.
	//
	// Vignette 0.45 -> 0.34. At 0.45 the corners of a frame whose average luminance is already very
	// low go to essentially nothing, which costs real information in a 5v5 - an enemy entering from
	// the edge of the screen is exactly the thing you need to see - and reads as a heavy filter
	// rather than as a lens. 0.34 still does the framing job on the big dark walls.
	PP.bOverride_ColorSaturation = true;
	PP.ColorSaturation = FVector4(1.18f, 1.18f, 1.18f, 1.f);
	PP.bOverride_VignetteIntensity = true;
	PP.VignetteIntensity = 0.34f;

	// Barely-there chromatic aberration: it stops the neon edges reading as perfectly clean vector
	// graphics.
	//
	// Cut 0.6 -> 0.2 during integration. Chromatic aberration works by sampling the R and B channels
	// at different radii, i.e. it SMEARS COLOUR ACROSS EDGES — and this scene is made of almost
	// nothing but coloured edges, so it was paying for its texture with the exact defect the user
	// reported ("it's also kinda blurry right now"). Measured on matched frames, dropping it was
	// worth +59% high-frequency detail on top of the TSR/native-resolution fixes in DefaultEngine.ini,
	// the largest single win left in the frame. Not zero: at 0 the floor grain roughly doubles,
	// because the fringe was also dithering the SSR noise in the near-black floor. 0.2 keeps that
	// and stops the smearing.
	PP.bOverride_SceneFringeIntensity = true;
	PP.SceneFringeIntensity = 0.2f;

	SpawnedActors.Add(Volume);
}

// -------------------------------------------------------------------------------------------------
// Primitive helpers
// -------------------------------------------------------------------------------------------------

EObjectFlags ATraceArenaBuilder::BuiltObjectFlags() const
{
	// RF_Transient during an editor preview and nothing at runtime. This is the flag that keeps the
	// preview out of the .umap: a transient object is skipped when the level is saved, so a user who
	// builds a preview and then saves the map saves the builder actor and none of its geometry.
	return bBuildingEditorPreview ? RF_Transient : RF_NoFlags;
}

UStaticMeshComponent* ATraceArenaBuilder::AddMeshBlock(UStaticMesh* Mesh, const FVector& LocalCenter, const FVector& Size,
	UMaterialInstanceDynamic* MID, bool bCastShadow, const TCHAR* DebugName, float YawDegrees)
{
	return AddMeshBlockRotated(Mesh, LocalCenter, Size, MID, bCastShadow, DebugName, FRotator(0.f, YawDegrees, 0.f));
}

UStaticMeshComponent* ATraceArenaBuilder::AddMeshBlockRotated(UStaticMesh* Mesh, const FVector& LocalCenter,
	const FVector& Size, UMaterialInstanceDynamic* MID, bool bCastShadow, const TCHAR* DebugName,
	const FRotator& Rotation)
{
	// Null-checked per the asset rules: a missing engine shape must degrade to "invisible", never
	// to a crash. Collision lives in separate box components, so the arena still plays.
	if (Mesh == nullptr || Root == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
		this, MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), FName(DebugName)),
		BuiltObjectFlags());
	if (Component == nullptr)
	{
		return nullptr;
	}

	// Everything is configured BEFORE RegisterComponent: a scene component may be freely posed
	// while unregistered, and registering once with the final state avoids a redundant render and
	// physics update per piece.
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetupAttachment(Root);
	Component->SetStaticMesh(Mesh);
	Component->SetRelativeLocation(LocalCenter);
	Component->SetRelativeRotation(Rotation);
	Component->SetRelativeScale3D(Size / TraceArenaConstants::ShapeUnit);
	Component->SetCollisionProfileName(TEXT("NoCollision"));
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(bCastShadow);

	if (MID != nullptr)
	{
		Component->SetMaterial(0, MID);
	}

	Component->RegisterComponent();
	return Component;
}

UBoxComponent* ATraceArenaBuilder::AddCollisionBlock(const FVector& LocalCenter, const FVector& Size, const TCHAR* DebugName,
	float YawDegrees)
{
	return AddCollisionBlockRotated(LocalCenter, Size, DebugName, FRotator(0.f, YawDegrees, 0.f));
}

UBoxComponent* ATraceArenaBuilder::AddCollisionBlockRotated(const FVector& LocalCenter, const FVector& Size,
	const TCHAR* DebugName, const FRotator& Rotation)
{
	if (Root == nullptr)
	{
		return nullptr;
	}

	UBoxComponent* Component = NewObject<UBoxComponent>(
		this, MakeUniqueObjectName(this, UBoxComponent::StaticClass(), FName(DebugName)),
		BuiltObjectFlags());
	if (Component == nullptr)
	{
		return nullptr;
	}

	Component->SetMobility(EComponentMobility::Movable);
	Component->SetupAttachment(Root);
	Component->SetRelativeLocation(LocalCenter);
	Component->SetRelativeRotation(Rotation);
	Component->SetBoxExtent(Size * 0.5f, /*bUpdateOverlaps=*/false);
	// BlockAll: WorldStatic object type blocking every channel. That single profile covers all
	// three things the arena has to stop - pawn movement sweeps, the Core's projectile sweeps and
	// hitscan line traces (ECC_Visibility) - without hand-rolling a response table.
	Component->SetCollisionProfileName(TEXT("BlockAll"));
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetHiddenInGame(true);

	Component->RegisterComponent();
	return Component;
}

UBoxComponent* ATraceArenaBuilder::AddPawnStandoff(const FVector& LocalCenter, const FVector& Size,
	const TCHAR* DebugName, float YawDegrees)
{
	if (Root == nullptr)
	{
		return nullptr;
	}

	UBoxComponent* Component = NewObject<UBoxComponent>(
		this, MakeUniqueObjectName(this, UBoxComponent::StaticClass(), FName(DebugName)),
		BuiltObjectFlags());
	if (Component == nullptr)
	{
		return nullptr;
	}

	Component->SetMobility(EComponentMobility::Movable);
	Component->SetupAttachment(Root);
	Component->SetRelativeLocation(LocalCenter);
	Component->SetRelativeRotation(FRotator(0.f, YawDegrees, 0.f));
	Component->SetBoxExtent(Size * 0.5f, /*bUpdateOverlaps=*/false);

	// PAWN ONLY, and that is the entire design. A second BlockAll box would have moved every
	// sightline, every bullet and every camera probe outward with it, which is a gameplay change
	// nobody asked for; this one moves only where a BODY may stand. Concretely:
	//   ECC_Visibility stays Ignore -> hitscan, bot line of sight and peek angles are untouched
	//   ECC_Camera     stays Ignore -> the third-person spring arm still hugs the real geometry
	//   everything else stays Ignore -> the Core, overlaps and traces pass straight through
	// QueryOnly because character movement is swept queries; there is no simulation to feed.
	Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Component->SetCollisionObjectType(ECC_WorldStatic);
	Component->SetCollisionResponseToAllChannels(ECR_Ignore);
	Component->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetHiddenInGame(true);

	Component->RegisterComponent();
	return Component;
}

void ATraceArenaBuilder::AddNeonBlock(const FVector& LocalCenter, const FVector& Size, float YawDegrees,
	UMaterialInstanceDynamic* BodyMID, UMaterialInstanceDynamic* NeonMID, bool bCollide, const TCHAR* DebugName,
	UMaterialInstanceDynamic* FaceNeonMID, bool bVerticalTrim, bool bFaceBands)
{
	// Whether this block gets face trim is decided from GEOMETRY, not from whether the materials
	// resolved, because a dedicated server passes null MIDs and still has to build byte-identical
	// collision to the clients predicting against it.
	//
	// It also decides whether the block gets a PAWN STANDOFF SHELL, which is why bFaceBands is in
	// the test as well as bVerticalTrim: a piece with no face trim has no emissive lying outside its
	// collision box except the top lip, and the pieces that pass bFaceBands=false - the corner bank
	// terraces - are WALKABLE. A shell on a terrace would leave players standing on 26 uu of thin
	// air out past its visible edge, which is a worse defect than the one the shell prevents.
	const bool bHasFaceTrim = bVerticalTrim && bFaceBands && (Size.Z >= TraceArenaConstants::FaceTrimMinHeight);

	if (bCollide)
	{
		AddCollisionBlock(LocalCenter, Size, DebugName, YawDegrees);

		// The pawn-only standoff shell. Only blocks that actually carry eye-height vertical trim
		// get one - which is exactly the blocks nobody can climb (the shortest is a 420 uu lane
		// rail against a ~90 uu jump apex). The stepped platforms take the other branch: they are
		// walkable, they carry no vertical trim, and a shell there would leave players standing on
		// 26 uu of thin air out past the visible edge.
		//
		// Not inflated in Z, for the same reason: the top face is a surface the game reasons about.
		if (bHasFaceTrim)
		{
			AddPawnStandoff(LocalCenter,
				FVector(Size.X + TraceArenaConstants::NeonStandoff * 2.f,
					Size.Y + TraceArenaConstants::NeonStandoff * 2.f,
					Size.Z),
				DebugName, YawDegrees);
		}
	}

	if (BodyMID == nullptr && NeonMID == nullptr)
	{
		return; // Dedicated server: collision only.
	}

	AddMeshBlock(CubeMesh, LocalCenter, Size, BodyMID, /*bCastShadow=*/true, DebugName, YawDegrees);

	// The glowing rim, sunk just under the top face so it reads as an inlaid light strip rather than
	// as a hat. Visual only - see the header for why it must never collide.
	const FVector LipCenter(LocalCenter.X, LocalCenter.Y,
		LocalCenter.Z + Size.Z * 0.5f - TraceArenaConstants::LipDrop - TraceArenaConstants::LipHeight * 0.5f);
	const FVector LipSize(
		Size.X + TraceArenaConstants::LipOut * 2.f,
		Size.Y + TraceArenaConstants::LipOut * 2.f,
		TraceArenaConstants::LipHeight);

	AddMeshBlock(CubeMesh, LipCenter, LipSize, NeonMID, /*bCastShadow=*/false, DebugName, YawDegrees);

	// --- Face trim -------------------------------------------------------------------------------
	//
	// Everything from here down exists so the block reads HEAD ON. See the header comment.

	if (NeonMID == nullptr || !bFaceBands || Size.Z < TraceArenaConstants::FaceTrimMinHeight)
	{
		return;
	}

	UMaterialInstanceDynamic* TrimMID = (FaceNeonMID != nullptr) ? FaceNeonMID : NeonMID;

	const float BottomZ = LocalCenter.Z - Size.Z * 0.5f;

	// Skirt: a glowing hem where the block meets the floor. On the near-mirror floor its reflection
	// runs out underneath the block, which is what tells you at a glance where the block's footprint
	// is - the single most useful thing to know about a piece of cover you are about to run behind.
	AddMeshBlock(CubeMesh,
		FVector(LocalCenter.X, LocalCenter.Y, BottomZ + TraceArenaConstants::SkirtRise + TraceArenaConstants::SkirtHeight * 0.5f),
		FVector(Size.X + TraceArenaConstants::SkirtOut * 2.f, Size.Y + TraceArenaConstants::SkirtOut * 2.f,
			TraceArenaConstants::SkirtHeight),
		TrimMID, /*bCastShadow=*/false, DebugName, YawDegrees);

	// Band: one line across the middle of the face. Two horizontal lines plus the lip give a face a
	// SCALE, which is what turns "a black shape" into "a block about my height, about there".
	AddMeshBlock(CubeMesh,
		FVector(LocalCenter.X, LocalCenter.Y, LocalCenter.Z),
		FVector(Size.X + TraceArenaConstants::BandOut * 2.f, Size.Y + TraceArenaConstants::BandOut * 2.f,
			TraceArenaConstants::BandHeight),
		TrimMID, /*bCastShadow=*/false, DebugName, YawDegrees);

	if (!bVerticalTrim)
	{
		return;
	}

	// Corner ribs. Centred exactly on each vertical corner, so half of each rib overhangs into the
	// air on both adjoining faces and the corner reads as a lit edge from every angle - the same
	// trick AddPylon uses, which is why the pylons were already legible when the cover blocks were
	// not. The rib stops below the lip and above the skirt so the three never fight for the same
	// pixels.
	const float RibBottom = BottomZ + TraceArenaConstants::SkirtRise + TraceArenaConstants::SkirtHeight;
	const float RibTop = LocalCenter.Z + Size.Z * 0.5f - TraceArenaConstants::LipDrop
		- TraceArenaConstants::LipHeight - TraceArenaConstants::CornerRibClearance;
	const float RibHeight = RibTop - RibBottom;

	if (RibHeight <= 0.f)
	{
		return;
	}

	// The block may be yawed (every cover block is a 45-degree diamond), so its corners have to be
	// rotated into the builder's frame by hand - AddMeshBlock's yaw only spins a piece about its own
	// centre.
	const float YawRadians = FMath::DegreesToRadians(YawDegrees);
	const float CosYaw = FMath::Cos(YawRadians);
	const float SinYaw = FMath::Sin(YawRadians);
	const float HalfSizeX = Size.X * 0.5f;
	const float HalfSizeY = Size.Y * 0.5f;

	// Eight ribs: one on each of the four corners, and one down the centre of each of the four faces.
	// See FaceRibSide for why the face-centre four are not redundant with the corners.
	struct FRibSpec
	{
		float OffsetX;
		float OffsetY;
		float Side;
	};

	const FRibSpec Ribs[] =
	{
		{ -HalfSizeX, -HalfSizeY, TraceArenaConstants::CornerRibSide },
		{ -HalfSizeX,  HalfSizeY, TraceArenaConstants::CornerRibSide },
		{  HalfSizeX, -HalfSizeY, TraceArenaConstants::CornerRibSide },
		{  HalfSizeX,  HalfSizeY, TraceArenaConstants::CornerRibSide },
		{ -HalfSizeX,        0.f, TraceArenaConstants::FaceRibSide },
		{  HalfSizeX,        0.f, TraceArenaConstants::FaceRibSide },
		{        0.f, -HalfSizeY, TraceArenaConstants::FaceRibSide },
		{        0.f,  HalfSizeY, TraceArenaConstants::FaceRibSide }
	};

	for (const FRibSpec& Rib : Ribs)
	{
		AddMeshBlock(CubeMesh,
			FVector(LocalCenter.X + Rib.OffsetX * CosYaw - Rib.OffsetY * SinYaw,
				LocalCenter.Y + Rib.OffsetX * SinYaw + Rib.OffsetY * CosYaw,
				RibBottom + RibHeight * 0.5f),
			FVector(Rib.Side, Rib.Side, RibHeight),
			TrimMID, /*bCastShadow=*/false, DebugName, YawDegrees);
	}
}

void ATraceArenaBuilder::AddWallButtress(const FVector2D& LocalCentre, const FVector2D& InwardAxis, float Width,
	float Depth, float Height, UMaterialInstanceDynamic* BodyMID, UMaterialInstanceDynamic* NeonMID,
	const TCHAR* DebugName)
{
	// InwardAxis is one of the four cardinals, so "which way does the width run" is just a question
	// of which component is non-zero. Written this way rather than with a yaw so the collision box
	// stays axis-aligned - the arena has thirty of these and an axis-aligned box is the cheapest
	// thing the physics scene can hold.
	const bool bAlongX = FMath::IsNearlyZero(InwardAxis.X);
	const FVector Size = bAlongX ? FVector(Width, Depth, Height) : FVector(Depth, Width, Height);
	const FVector Centre(LocalCentre.X, LocalCentre.Y, Height * 0.5f);

	AddCollisionBlock(Centre, Size, DebugName);

	// Thirty of these stand flush against the perimeter walls carrying full-height neon at
	// GlowPylon, which made them the brightest thing a player could put their face into. Same
	// pawn-only shell as everything else; see the standoff note in TraceArenaConstants. Inflating it
	// on all four sides pushes part of the shell into the wall, which is harmless.
	AddPawnStandoff(Centre,
		FVector(Size.X + TraceArenaConstants::NeonStandoff * 2.f,
			Size.Y + TraceArenaConstants::NeonStandoff * 2.f, Size.Z),
		DebugName);

	if (BodyMID == nullptr && NeonMID == nullptr)
	{
		return;   // Dedicated server: collision only.
	}

	AddMeshBlock(CubeMesh, Centre, Size, BodyMID, /*bCastShadow=*/false, DebugName);

	if (NeonMID == nullptr)
	{
		return;
	}

	// TWO narrow strips on the face that looks back into the field, not one wide one.
	//
	// It was a single strip at Width * 0.26 - 109 uu on a 420 uu buttress - standing 22 uu PROUD of
	// the collision box, so an eye could get 12 uu from it. That is 109 uu of unlit emissive at
	// GlowPylon filling 155 deg of a 95 deg frame: the worst whiteout in the arena, thirty times
	// over. Splitting it into two 32 uu lines flush with the face fixes both halves of that at once
	// - each line subtends 30 deg at the closest an eye can now get, they are 200 uu apart so they
	// can never both dominate the frame, and a buttress with two lit edges and a dark channel
	// between them reads as a pilaster instead of as a glowing slab.
	//
	// The other three faces are still bare: they are either buried in the wall or seen edge-on from
	// inside the arena, so they would be pure cost.
	const float StripDepth = 24.f;
	const float StripWidth = FMath::Clamp(Width * 0.085f, 22.f, 32.f);
	const float StripHeight = FMath::Max(40.f, Height - 70.f);
	const float StripLateral = Width * 0.24f;

	// Flush: the strip is sunk into the buttress face with 1 uu proud, so nothing of it lies outside
	// the shell built above.
	const float FaceOut = Depth * 0.5f - StripDepth * 0.5f + 1.f;
	const FVector StripSize = bAlongX
		? FVector(StripWidth, StripDepth, StripHeight)
		: FVector(StripDepth, StripWidth, StripHeight);

	for (const float LateralSign : { -1.f, 1.f })
	{
		// The strips run along the buttress WIDTH, which is whichever horizontal axis the inward
		// axis is not.
		const FVector2D Lateral = bAlongX
			? FVector2D(LateralSign * StripLateral, 0.f)
			: FVector2D(0.f, LateralSign * StripLateral);

		AddMeshBlock(CubeMesh,
			FVector(Centre.X + InwardAxis.X * FaceOut + Lateral.X,
				Centre.Y + InwardAxis.Y * FaceOut + Lateral.Y,
				StripHeight * 0.5f + 30.f),
			StripSize, NeonMID, /*bCastShadow=*/false, DebugName);
	}
}

UPointLightComponent* ATraceArenaBuilder::AddPointLight(const FVector& LocalCenter, float AttenuationRadius,
	float Intensity, const FLinearColor& Color, const TCHAR* DebugName)
{
	if (Root == nullptr)
	{
		return nullptr;
	}

	UPointLightComponent* Light = NewObject<UPointLightComponent>(
		this, MakeUniqueObjectName(this, UPointLightComponent::StaticClass(), FName(DebugName)),
		BuiltObjectFlags());
	if (Light == nullptr)
	{
		return nullptr;
	}

	// Movable for the same reason every other light in this file is: the project disables static
	// lighting outright (r.AllowStaticLighting=False) and nothing here has baked data.
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetupAttachment(Root);
	Light->SetRelativeLocation(LocalCenter);
	Light->SetIntensity(Intensity);
	Light->SetLightColor(Color);
	Light->SetAttenuationRadius(AttenuationRadius);

	// NO SHADOWS, and this is not a performance nicety - it is the point. Twenty-four shadow-casting
	// point lights would be twenty-four extra depth passes AND would have every cover block casting
	// four overlapping shadows across the floor, which on a near-black floor is just noise. What
	// these lights are for is the light TERM on vertical faces; the shape reading comes from the
	// neon trim, and the one shadow that matters (the key light's) is untouched.
	Light->SetCastShadows(false);
	Light->SetVolumetricScatteringIntensity(0.f);

	Light->RegisterComponent();
	return Light;
}

void ATraceArenaBuilder::BuildFloorLamps()
{
	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();

	if (FloorLampIntensity <= 0.f || FloorLampRadius <= 0.f)
	{
		return;
	}

	for (const float XFrac : TraceArenaConstants::LampXFracs)
	{
		for (const float XSign : { -1.f, 1.f })
		{
			// Half-tinted, but only halfway there. A fully saturated team-coloured lamp repaints every
			// surface it touches and the two halves stop looking like the same arena; blending to
			// white keeps the tint as a hint and lets the neon keep ownership of the colour.
			const ETraceTeam HalfTeam = (XSign < 0.f) ? ETraceTeam::Blue : ETraceTeam::Orange;
			const FLinearColor TeamColor = TraceTeamColor(HalfTeam);
			const FLinearColor LampColor(
				FMath::Lerp(0.72f, TeamColor.R, 0.55f),
				FMath::Lerp(0.78f, TeamColor.G, 0.55f),
				FMath::Lerp(1.00f, TeamColor.B, 0.55f));

			for (const float YFrac : TraceArenaConstants::LampYFracs)
			{
				for (const float YSign : { -1.f, 1.f })
				{
					AddPointLight(
						FVector(XSign * HalfX * XFrac, YSign * HalfY * YFrac, TraceArenaConstants::LampZ),
						FloorLampRadius, FloorLampIntensity, LampColor, TEXT("FloorLamp"));
				}
			}
		}
	}
}

void ATraceArenaBuilder::AddSteppedPlatform(const FVector2D& LocalCentre, float TopTierSide, float TierSideStep,
	int32 TierCount, float YawDegrees, UMaterialInstanceDynamic* BodyMID, UMaterialInstanceDynamic* NeonMID,
	const TCHAR* DebugName)
{
	const int32 Tiers = FMath::Clamp(TierCount, 1, 12);

	// Tier 0 is the widest and shortest. Each tier is a solid block from the floor up to its own top
	// rather than a slab balanced on the one below, so the whole platform is a nest of boxes: no
	// seams for a capsule to catch on, and every riser is exactly StepRise regardless of tier.
	for (int32 Tier = 0; Tier < Tiers; ++Tier)
	{
		const float Side = TopTierSide + TierSideStep * static_cast<float>(Tiers - 1 - Tier);
		const float TopZ = TraceArenaConstants::StepRise * static_cast<float>(Tier + 1);

		// bVerticalTrim=false: a tier's exposed riser is one StepRise (40 uu) tall, so a corner rib
		// would be a 40 uu stub, and there are four of them per tier. The lip already carries it.
		AddNeonBlock(FVector(LocalCentre.X, LocalCentre.Y, TopZ * 0.5f),
			FVector(Side, Side, TopZ), YawDegrees, BodyMID, NeonMID, /*bCollide=*/true, DebugName,
			/*FaceNeonMID=*/nullptr, /*bVerticalTrim=*/false);
	}

	if (NeonMID == nullptr)
	{
		return; // Dedicated server: the tiers above contributed their collision, and that is all it needs.
	}

	// Two crossed strips inlaid in the top face.
	//
	// The top tier is the one large unbroken horizontal surface on the platform, and up-facing
	// surfaces catch the most key light, so without this it reads as a blank pale slab - measured,
	// it was the one thing in the arena that still looked like untextured greybox. The cross also
	// marks the centre of the high ground, which is where the fight for it happens.
	const float TopSide = TopTierSide;
	const float TopFaceZ = TraceArenaConstants::StepRise * static_cast<float>(Tiers) + TraceArenaConstants::GridThickness * 0.5f;
	const float InlayLength = TopSide * 0.86f;
	const float InlayWidth = FMath::Max(14.f, TopSide * 0.018f);

	AddMeshBlock(CubeMesh, FVector(LocalCentre.X, LocalCentre.Y, TopFaceZ),
		FVector(InlayLength, InlayWidth, TraceArenaConstants::GridThickness), NeonMID, false, DebugName, YawDegrees);
	AddMeshBlock(CubeMesh, FVector(LocalCentre.X, LocalCentre.Y, TopFaceZ),
		FVector(InlayWidth, InlayLength, TraceArenaConstants::GridThickness), NeonMID, false, DebugName, YawDegrees);
}

void ATraceArenaBuilder::AddPylon(const FVector2D& LocalCentre, float Side, float Height,
	UMaterialInstanceDynamic* BodyMID, UMaterialInstanceDynamic* NeonMID, const TCHAR* DebugName)
{
	const FVector Centre(LocalCentre.X, LocalCentre.Y, Height * 0.5f);

	AddCollisionBlock(Centre, FVector(Side, Side, Height), DebugName);

	// Every pylon in the arena carries full-height neon on all four faces, so every one of them is a
	// point-blank whiteout candidate. See the standoff note in TraceArenaConstants.
	AddPawnStandoff(Centre,
		FVector(Side + TraceArenaConstants::NeonStandoff * 2.f,
			Side + TraceArenaConstants::NeonStandoff * 2.f, Height),
		DebugName);

	if (BodyMID == nullptr && NeonMID == nullptr)
	{
		return;
	}

	AddMeshBlock(CubeMesh, Centre, FVector(Side, Side, Height), BodyMID, /*bCastShadow=*/true, DebugName);

	// --- Base and capital ------------------------------------------------------------------------
	//
	// REFINEMENT, not decoration. An extruded square prism is the single most greybox shape there
	// is: it has no scale, no top and no bottom, and eleven of them stand in this arena as its
	// landmarks. A wider plinth at the foot and a narrower head at the top give the column a
	// direction and a place where it meets the floor, which is most of what separates "a building"
	// from "a box". Both are kept inside NeonStandoff (22 and 15 uu proud respectively) so neither
	// pokes out through the shell above.
	const float BaseHeight = 56.f;
	const float CapHeight = 46.f;
	const float BaseSide = Side + 44.f;
	const float CapSide = Side + 30.f;

	AddMeshBlock(CubeMesh, FVector(Centre.X, Centre.Y, BaseHeight * 0.5f),
		FVector(BaseSide, BaseSide, BaseHeight), BodyMID, /*bCastShadow=*/true, DebugName);
	AddMeshBlock(CubeMesh, FVector(Centre.X, Centre.Y, Height - CapHeight * 0.5f),
		FVector(CapSide, CapSide, CapHeight), BodyMID, /*bCastShadow=*/true, DebugName);

	if (NeonMID == nullptr)
	{
		return;
	}

	// A neon collar just under the capital. It is the line that reads from the far endzone.
	AddMeshBlock(CubeMesh, FVector(Centre.X, Centre.Y, Height - CapHeight - 11.f),
		FVector(Side + 18.f, Side + 18.f, 14.f), NeonMID, /*bCastShadow=*/false, DebugName);

	// --- Face strips -----------------------------------------------------------------------------
	//
	// Neon on each of the four faces is what makes a pylon read as a light column from any angle
	// instead of only from the side that happens to be facing you, and these columns are the arena's
	// landmarks, so being readable from everywhere is the job.
	//
	// TWO CHANGES FROM THE FIRST PASS, both from the point-blank measurement:
	//   RECESSED, not proud. The strip used to sit Side*0.5 + StripDepth*0.5 - 6 out, i.e. 20 uu
	//   OUTSIDE the collision box, so an eye stopped 34 uu from the box was 14 uu from the strip.
	//   It is now sunk into the face with 1 uu of proud, so the shell above governs the distance.
	//   CAPPED WIDTH, and doubled where the face is wide. Side*0.22 gave a 92 uu strip on a 420 uu
	//   gate tower; at any reachable distance that is a light panel, not a neon line. Faces wide
	//   enough to look bare with one 36 uu strip get two with a dark gap between them, which reads
	//   as more structure, not less, and never puts a single sheet of emissive across the frame.
	const float StripWidth = FMath::Clamp(Side * 0.16f, 22.f, TraceArenaConstants::MaxNeonStripWidth);
	const float StripDepth = 26.f;
	const float StripInset = Side * 0.5f - StripDepth * 0.5f + 1.f;
	const float StripBottom = BaseHeight + 12.f;
	const float StripTop = Height - CapHeight - 30.f;
	const float StripHeight = StripTop - StripBottom;
	const float StripZ = (StripTop + StripBottom) * 0.5f;

	if (StripHeight <= 0.f)
	{
		return;
	}

	// One strip on the face centre, or two flanking it once the face is wide enough that a single
	// line looks lost on it.
	TArray<float, TInlineAllocator<2>> LateralOffsets;
	if (Side >= 320.f)
	{
		LateralOffsets.Add(-Side * 0.26f);
		LateralOffsets.Add(Side * 0.26f);
	}
	else
	{
		LateralOffsets.Add(0.f);
	}

	for (const float Lateral : LateralOffsets)
	{
		AddMeshBlock(CubeMesh, FVector(Centre.X + StripInset, Centre.Y + Lateral, StripZ),
			FVector(StripDepth, StripWidth, StripHeight), NeonMID, false, DebugName);
		AddMeshBlock(CubeMesh, FVector(Centre.X - StripInset, Centre.Y + Lateral, StripZ),
			FVector(StripDepth, StripWidth, StripHeight), NeonMID, false, DebugName);
		AddMeshBlock(CubeMesh, FVector(Centre.X + Lateral, Centre.Y + StripInset, StripZ),
			FVector(StripWidth, StripDepth, StripHeight), NeonMID, false, DebugName);
		AddMeshBlock(CubeMesh, FVector(Centre.X + Lateral, Centre.Y - StripInset, StripZ),
			FVector(StripWidth, StripDepth, StripHeight), NeonMID, false, DebugName);
	}
}

UMaterialInstanceDynamic* ATraceArenaBuilder::MakeSurfaceMID(const FLinearColor& BaseColor, float Roughness,
	float Metallic, const FLinearColor& Emissive, float EmissiveStrength)
{
	UMaterialInterface* Parent = (SurfaceMaterial != nullptr) ? SurfaceMaterial.Get() : BaseMaterial.Get();
	if (Parent == nullptr)
	{
		return nullptr;
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, this);
	if (MID == nullptr)
	{
		return nullptr;
	}

	// M_TraceSurface's parameters. "Color" is BasicShapeMaterial's name for the same thing and is a
	// harmless no-op on M_TraceSurface, so one call site covers both the real material and the
	// fallback.
	MID->SetVectorParameterValue(TEXT("BaseColor"), BaseColor);
	MID->SetVectorParameterValue(TEXT("Color"), BaseColor);
	MID->SetScalarParameterValue(TEXT("Roughness"), FMath::Clamp(Roughness, 0.f, 1.f));
	MID->SetScalarParameterValue(TEXT("Metallic"), FMath::Clamp(Metallic, 0.f, 1.f));
	MID->SetVectorParameterValue(TEXT("Emissive"), Emissive);
	MID->SetScalarParameterValue(TEXT("EmissiveStrength"), EmissiveStrength);

	// Outered to this actor, so it follows the same transient rule as the components that use it.
	MID->SetFlags(BuiltObjectFlags());

	TintMIDs.Add(MID);
	return MID;
}

UMaterialInstanceDynamic* ATraceArenaBuilder::MakeNeonMID(const FLinearColor& Color, float Glow)
{
	const bool bHaveNeon = (NeonMaterial != nullptr);
	UMaterialInterface* Parent = bHaveNeon ? NeonMaterial.Get() : BaseMaterial.Get();
	if (Parent == nullptr)
	{
		return nullptr;
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, this);
	if (MID == nullptr)
	{
		return nullptr;
	}

	MID->SetVectorParameterValue(TEXT("Color"), Color);
	MID->SetScalarParameterValue(TEXT("Glow"), FMath::Max(0.f, Glow));

	if (!bHaveNeon)
	{
		// Fallback path: BasicShapeMaterial is lit and has no Glow, so the best available
		// approximation is a bright matte albedo. It will not bloom and it will not read as neon -
		// that is the cost of not having run Scripts/generate_content.py.
		MID->SetScalarParameterValue(TEXT("Roughness"), 0.9f);
	}

	MID->SetFlags(BuiltObjectFlags());

	TintMIDs.Add(MID);
	return MID;
}

// -------------------------------------------------------------------------------------------------
// Derived layout
// -------------------------------------------------------------------------------------------------

float ATraceArenaBuilder::TeamEndSign(ETraceTeam Team)
{
	// BUILD-TIME PAINT ONLY. Blue's colour goes on the -X end and Orange's on +X. This is no longer
	// a statement about who defends what: spec §1 switches sides at half time and
	// ATraceGameState::GetDefendedEndSign() is the runtime authority. ApplyTeamSides() below is what
	// keeps the paint honest afterwards.
	return (Team == ETraceTeam::Blue) ? -1.f : 1.f;
}

// -------------------------------------------------------------------------------------------------
// Half-time side switch (spec §1)
// -------------------------------------------------------------------------------------------------

void ATraceArenaBuilder::RegisterSideMID(float EndSign, UMaterialInstanceDynamic* MID, bool bNeon,
	float Intensity, float BaseDim)
{
	if (MID == nullptr || FMath::IsNearlyZero(EndSign))
	{
		return;
	}

	FTraceSideMID& Entry = SideMIDs.AddDefaulted_GetRef();
	Entry.EndSign = (EndSign < 0.f) ? -1.f : 1.f;
	Entry.MID = MID;
	Entry.bNeon = bNeon;
	Entry.Intensity = Intensity;
	Entry.BaseDim = BaseDim;
}

void ATraceArenaBuilder::ApplyTeamSides(ETraceTeam TeamOnNegativeSide)
{
	// None is not a side assignment; ignore it rather than painting the arena black.
	if (TeamOnNegativeSide == ETraceTeam::None)
	{
		return;
	}

	// Idempotent by construction (the colours are re-derived, never toggled), but skipping the
	// redundant case keeps this off the hot path if it is ever called from a tick.
	if (bArenaBuilt && TeamOnNegativeSide == PaintedTeamOnNegativeSide && SideMIDs.Num() > 0)
	{
		return;
	}
	PaintedTeamOnNegativeSide = TeamOnNegativeSide;

	const ETraceTeam TeamOnPositiveSide =
		(TeamOnNegativeSide == ETraceTeam::Blue) ? ETraceTeam::Orange : ETraceTeam::Blue;

	int32 Repainted = 0;
	for (const FTraceSideMID& Entry : SideMIDs)
	{
		UMaterialInstanceDynamic* MID = Entry.MID.Get();
		if (MID == nullptr)
		{
			continue;   // The component that owned it was destroyed; nothing to repaint.
		}

		const ETraceTeam DefendingTeam = (Entry.EndSign < 0.f) ? TeamOnNegativeSide : TeamOnPositiveSide;
		const FLinearColor TeamColor = TraceTeamColor(DefendingTeam);

		if (Entry.bNeon)
		{
			// Brightness rides the Glow SCALAR, never the Color vector: a material instance clamps
			// vector parameters to [0,1], so pushing a pre-multiplied colour would silently flatten
			// every one of these to white at high intensity.
			MID->SetVectorParameterValue(TEXT("Color"), TeamColor);
			MID->SetScalarParameterValue(TEXT("Glow"), FMath::Max(0.f, Entry.Intensity));
		}
		else
		{
			// BaseDim < 0 is the sentinel for "the base colour is structural, not the team's" (the
			// gate towers); only the emissive term carries the team on those.
			if (Entry.BaseDim >= 0.f)
			{
				const FLinearColor BaseColor = TraceArenaConstants::Dim(TeamColor, Entry.BaseDim);
				MID->SetVectorParameterValue(TEXT("BaseColor"), BaseColor);
				MID->SetVectorParameterValue(TEXT("Color"), BaseColor);
			}
			MID->SetVectorParameterValue(TEXT("Emissive"), TeamColor);
			MID->SetScalarParameterValue(TEXT("EmissiveStrength"), FMath::Max(0.f, Entry.Intensity));
		}

		++Repainted;
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[Arena] Sides applied: %s now defends -X. Repainted %d of %d surfaces."),
		*TraceTeamName(TeamOnNegativeSide).ToString(), Repainted, SideMIDs.Num());
}

// -------------------------------------------------------------------------------------------------
// Scoring mode (spec v4 section 7) - both shapes built, one presented
// -------------------------------------------------------------------------------------------------

int32 ATraceArenaBuilder::MarkBuiltComponents() const
{
	return (Root != nullptr) ? Root->GetAttachChildren().Num() : 0;
}

void ATraceArenaBuilder::CollectPiecesSince(int32 Mark, TArray<FTraceModePiece>& Out) const
{
	if (Root == nullptr)
	{
		return;
	}

	// MARK, BUILD, DIFF - and no primitive helper had to learn that modes exist. Every factory in
	// this file does SetupAttachment(Root) and AttachChildren appends in construction order, so the
	// components added since the mark ARE what the step built. The alternative was threading a
	// "which mode is this for" argument through AddMeshBlock / AddCollisionBlock / AddPawnStandoff /
	// AddNeonBlock / AddPylon, which is five signatures and a default argument nobody would notice
	// getting wrong.
	const auto& BuiltChildren = Root->GetAttachChildren();
	for (int32 Index = FMath::Max(0, Mark); Index < BuiltChildren.Num(); ++Index)
	{
		USceneComponent* Built = BuiltChildren[Index];
		if (!IsValid(Built))
		{
			continue;
		}

		FTraceModePiece& Piece = Out.AddDefaulted_GetRef();
		Piece.Component = Built;

		// Remembered rather than assumed, so re-arming restores what the piece was BUILT with. The
		// goal posts are BlockAll boxes, their standoff shells are QueryOnly pawn-blockers and the
		// paint is NoCollision: three different answers in one tagged set.
		if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Built))
		{
			Piece.Collision = Primitive->GetCollisionEnabled();
		}
	}
}

void ATraceArenaBuilder::SetPiecesPresented(const TArray<FTraceModePiece>& Pieces, bool bPresented)
{
	for (const FTraceModePiece& Piece : Pieces)
	{
		USceneComponent* Component = Piece.Component.Get();
		if (!IsValid(Component))
		{
			continue;
		}

		Component->SetVisibility(bPresented, /*bPropagateToChildren=*/true);

		// Visibility alone is not enough and this is the important half: an invisible goal post that
		// still blocks is an invisible wall standing in the endzone for the whole of mode A, which is
		// the worst kind of bug - it has no visual symptom at all.
		if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
		{
			Primitive->SetCollisionEnabled(bPresented ? Piece.Collision.GetValue() : ECollisionEnabled::NoCollision);
		}
	}
}

void ATraceArenaBuilder::ApplyScoringMode(ETraceScoringMode NewMode)
{
	ScoringMode = NewMode;

	// Legal before the build: the mode is remembered above and BuildArena calls this again at the
	// end, so an early call (the game mode publishing before the arena exists) is not a lost edit.
	if (!bArenaBuilt)
	{
		return;
	}

	const bool bGoalMode = TraceIsGoalMode(ScoringMode);

	SetPiecesPresented(EndzoneModePieces, !bGoalMode);
	SetPiecesPresented(GoalModePieces, bGoalMode);

	// Arm one pair of volumes, disarm the other. Empty on clients (no scoring volume is ever spawned
	// there), which is correct: a client has nothing to arm.
	int32 Armed = 0;
	for (const TWeakObjectPtr<ATraceEndzone>& Weak : ScoringVolumes)
	{
		ATraceEndzone* Zone = Weak.Get();
		if (!IsValid(Zone))
		{
			continue;
		}

		const bool bWanted = (Zone->IsGoalVolume() == bGoalMode);
		Zone->SetZoneActive(bWanted);
		Armed += bWanted ? 1 : 0;
	}

	// Display, not Log: this is the answer to "which game am I looking at", and on an A/B toggle
	// that is the first thing anybody reading a log wants to see.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Arena] Presenting %s: %d endzone pieces %s, %d goal pieces %s, %d of %d volumes armed."),
		*TraceScoringModeLabel(ScoringMode),
		EndzoneModePieces.Num(), bGoalMode ? TEXT("hidden") : TEXT("shown"),
		GoalModePieces.Num(), bGoalMode ? TEXT("shown") : TEXT("hidden"),
		Armed, ScoringVolumes.Num());
}

void ATraceArenaBuilder::ApplyScoringModeInWorld(const UWorld* World, ETraceScoringMode NewMode)
{
	if (World == nullptr)
	{
		return;
	}

	// One per world, and not replicated, so the server and every client has to be driven separately -
	// the same reason ApplyTeamSidesInWorld exists in this shape.
	for (TActorIterator<ATraceArenaBuilder> It(const_cast<UWorld*>(World)); It; ++It)
	{
		It->ApplyScoringMode(NewMode);
	}
}

void ATraceArenaBuilder::ApplyTeamSidesInWorld(const UWorld* World, ETraceTeam TeamOnNegativeSide)
{
	if (World == nullptr)
	{
		return;
	}

	// The builder is NOT replicated — every machine constructs its own arena from the same
	// parameters — so this has to be driven independently on the server and on each client. There is
	// deliberately no "find the one authoritative builder" here; there is one per world.
	for (TActorIterator<ATraceArenaBuilder> It(const_cast<UWorld*>(World)); It; ++It)
	{
		It->ApplyTeamSides(TeamOnNegativeSide);
	}
}

// -------------------------------------------------------------------------------------------------
// Editor preview
//
// See the two-click workflow documented on BuildPreviewInEditor in the header. Everything below is
// #if WITH_EDITOR: none of it is compiled into a packaged game, so a preview cannot exist there and
// the runtime path is byte-identical to what it was.
// -------------------------------------------------------------------------------------------------

#if WITH_EDITOR

void ATraceArenaBuilder::DestroyBuiltArena()
{
	// Every piece of geometry is a component attached under Root (AddMeshBlock, AddCollisionBlock,
	// AddPawnStandoff and AddPointLight all SetupAttachment(Root)), so walking the attachment tree
	// is a complete list by construction — no parallel bookkeeping array to fall out of step with
	// what was actually built. Root itself is a constructor subobject and stays.
	//
	// NOT named "Children": AActor has a member of that name and a local shadowing it fails the
	// Windows build under /W4 (C4458), which this project has already been bitten by once.
	int32 ComponentsDestroyed = 0;
	if (Root != nullptr)
	{
		TArray<USceneComponent*> BuiltChildren;
		Root->GetChildrenComponents(/*bIncludeAllDescendants=*/true, BuiltChildren);

		for (USceneComponent* Child : BuiltChildren)
		{
			if (IsValid(Child))
			{
				Child->DestroyComponent();
				++ComponentsDestroyed;
			}
		}
	}

	int32 ActorsDestroyed = 0;
	for (const TObjectPtr<AActor>& Spawned : SpawnedActors)
	{
		AActor* Actor = Spawned.Get();
		if (IsValid(Actor))
		{
			Actor->Destroy();
			++ActorsDestroyed;
		}
	}
	SpawnedActors.Reset();

	// The MIDs are outered to this actor and referenced only from here and from the components that
	// just died, so dropping the array is enough; GC takes them.
	TintMIDs.Reset();
	SideMIDs.Reset();
	PaintedTeamOnNegativeSide = ETraceTeam::Blue;

	// The mode tags point at components that have just been destroyed. Weak pointers make that safe
	// rather than fatal, but a stale entry would still make the next ApplyScoringMode walk a list of
	// nulls and report a component count that no longer means anything.
	EndzoneModePieces.Reset();
	GoalModePieces.Reset();
	ScoringVolumes.Reset();

	bArenaBuilt = false;
	bEditorPreviewBuilt = false;

	UE_LOG(LogTraceGame, Verbose, TEXT("ATraceArenaBuilder: torn down %d components and %d actors."),
		ComponentsDestroyed, ActorsDestroyed);
}

void ATraceArenaBuilder::BuildPreviewInEditor()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// EDITOR WORLDS ONLY. In a PIE or game world the arena is already built (or about to be) by the
	// game mode, and a second copy from a button press would be two floors, two sets of endzone
	// triggers and two lighting rigs on top of each other. Refuse loudly rather than half-work.
	if (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::EditorPreview)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("BuildPreviewInEditor is an editor-only tool and does nothing in a play session — the arena ")
			TEXT("builds itself at BeginPlay. Stop PIE, then press it in the level editor."));
		return;
	}

	// Idempotent: pressing the button twice, or pressing it after changing FieldLength, replaces the
	// preview rather than stacking a second arena on top of the first.
	DestroyBuiltArena();

	bBuildingEditorPreview = true;
	BuildArena();
	bBuildingEditorPreview = false;

	bEditorPreviewBuilt = true;

	// SELF-CHECK, and it earns its keep: "the preview cannot be saved into the map" is a claim about
	// an object flag on ~1000 objects, made by code that a later edit could quietly break (one
	// NewObject that forgets BuiltObjectFlags is all it takes), and the symptom would not appear
	// until someone saved the level and then played it with two arenas in it. Counting is cheap;
	// finding that bug from the symptom is not.
	int32 NonTransient = 0;
	for (const UActorComponent* Built : GetComponents())
	{
		if (Built != nullptr && Built != Root && !Built->HasAnyFlags(RF_Transient))
		{
			++NonTransient;
		}
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[Arena] Editor preview built: %d components, %d actors, %.0f x %.0f uu. ")
		TEXT("Press Clear Preview In Editor to remove it; it is never saved into the map."),
		GetComponents().Num(), SpawnedActors.Num(), FieldLength, FieldWidth);

	if (NonTransient > 0)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Arena] %d preview components are NOT transient and would be saved into the level, ")
			TEXT("which would double the arena at runtime. Every component factory must pass BuiltObjectFlags()."),
			NonTransient);
	}
	else
	{
		UE_LOG(LogTraceGame, Log, TEXT("[Arena] Preview is fully transient: nothing here can be saved into the map."));
	}
}

void ATraceArenaBuilder::ClearPreviewInEditor()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// Same gate as the build button, and for a much sharper reason: without it, pressing Clear on a
	// builder selected in the level while PIE is running would delete a LIVE arena out from under
	// the players standing in it.
	if (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::EditorPreview)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("ClearPreviewInEditor is an editor-only tool and does nothing in a play session."));
		return;
	}

	if (!bEditorPreviewBuilt && !bArenaBuilt)
	{
		UE_LOG(LogTraceGame, Log, TEXT("[Arena] No editor preview to clear."));
		return;
	}

	DestroyBuiltArena();
	UE_LOG(LogTraceGame, Log, TEXT("[Arena] Editor preview cleared."));
}

void ATraceArenaBuilder::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// A preview that does not follow the numbers next to it is a trap: you widen the field, the
	// viewport does not move, and you conclude the property does nothing. Only rebuild when a
	// preview is already standing (so this costs nothing for anyone not using the tool) and only on
	// a committed edit — EPropertyChangeType::Interactive fires on every mouse-drag frame of a
	// slider, and rebuilding ~830 components per frame would lock the editor up.
	if (bEditorPreviewBuilt && PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		BuildPreviewInEditor();
	}
}

#endif // WITH_EDITOR
