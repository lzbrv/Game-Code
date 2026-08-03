// Copyright (c) Trace. All Rights Reserved.

#include "World/TraceArenaBuilder.h"

#include "Trace.h"
#include "TraceTypes.h"
#include "Gameplay/TraceEndzone.h"
#include "World/TraceTeamPlayerStart.h"

#include "Components/BoxComponent.h"
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

	/** Tiers in the centre dais. Its top surface is therefore at StepRise * DaisTiers. */
	static constexpr int32 DaisTiers = 4;
	static constexpr float DaisTopTierSide = 1500.f;
	static constexpr float DaisTierSideStep = 700.f;

	/** Tiers in each of the four wing platforms. */
	static constexpr int32 WingTiers = 2;
	static constexpr float WingTopTierSide = 1700.f;
	static constexpr float WingTierSideStep = 700.f;

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
	 * 240 sits comfortably above StepRise * 4 (the tallest dais tier's own slab is the full 160) and
	 * comfortably below the 420 uu lane rails, which are the shortest thing that genuinely blocks a
	 * sightline and therefore genuinely has to be visible head-on.
	 */
	static constexpr float FaceTrimMinHeight = 240.f;

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

	/** Fraction of the half width the spawn fan covers. */
	static constexpr float StartSpreadFraction = 0.7f;

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

	// --- Interior layout -------------------------------------------------------------------------
	//
	// Everything below is a FRACTION of HalfLength() / HalfWidth(), so the whole arena rescales
	// coherently when FieldLength / FieldWidth change. The absolute numbers in the comments are for
	// the shipped 24000 x 12000 field.
	//
	// The design, from the centreline out:
	//   |Y| < 2300     central spine - the direct route, dominated by the raised dais
	//   |Y| ~ 2300     lane rail row A, segmented, splits spine from the mid lane
	//   |Y| ~ 4400     lane rail row B, segmented, splits mid lane from the outer lane
	//   |Y| ~ 4200     the four wing platforms, high ground overlooking the outer lanes
	// with diamond cover blocks scattered so that no straight line from a spawn to the dais, or from
	// the dais to either goal, is clear for its whole length. That is the point of the whole layout:
	// the previous arena was a flat empty rectangle and every duel was decided the instant it began.

	struct FCoverSpec
	{
		float XFrac;    // of HalfLength
		float YFrac;    // of HalfWidth
		float Side;     // uu, before the 45 degree rotation
		float Height;   // uu
	};

	/**
	 * Mirrored into all four quadrants. Every position here was checked against the rails, the wing
	 * platforms, the endzone gates and the player-start line so nothing is built inside anything
	 * else and nobody spawns inside a block - re-check if you move one.
	 */
	static const FCoverSpec CoverBlocks[] =
	{
		{ 0.2917f, 0.2167f,  950.f, 800.f },   // (3500, 1300) - spine, first sightline break
		{ 0.5750f, 0.2500f,  800.f, 680.f },   // (6900, 1500) - spine, mid field
		{ 0.3917f, 0.8750f,  900.f, 860.f },   // (4700, 5250) - outer lane
		{ 0.7250f, 0.5667f,  800.f, 760.f }    // (8700, 3400) - approach to the endzone
	};

	/** Mirrored in X only: two slabs straddling the centreline of each half. */
	static constexpr float AxisCoverXFrac = 0.4917f;   // 5900
	static const FVector AxisCoverSize(1600.f, 600.f, 900.f);

	/** Lane rails: Y position as a fraction of HalfWidth, and the segment centres along X. */
	static constexpr float RailRowAYFrac = 0.3833f;    // 2300
	static constexpr float RailRowBYFrac = 0.7333f;    // 4400
	static constexpr float RailRowAXFracs[] = { 0.2167f, 0.5500f };   // 2600, 6600
	static constexpr float RailRowBXFracs[] = { 0.1167f, 0.3000f };   // 1400, 3600
	static constexpr float RailLength = 2200.f;
	static constexpr float RailThickness = 260.f;
	static constexpr float RailHeight = 420.f;

	/** Wing platforms and their light pylons. */
	static constexpr float WingXFrac = 0.5333f;        // 6400
	static constexpr float WingYFrac = 0.7000f;        // 4200
	/**
	 * The mid-lane pylons, one per quadrant.
	 *
	 * These deliberately do NOT stand on the wing platforms. They did in the first pass, at the wing
	 * centre, and a screenshot from the platform top showed the result immediately: a 220 uu column
	 * planted in the middle of the one piece of high ground on that side of the field, so the reward
	 * for taking the high ground was a pillar in your face. Out here they still mark the lane and
	 * still break a sightline, and the platform top is clear to fight on.
	 *
	 * Position checked against tier 0 of the wing (2200 uu clear), both rail rows, cover blocks 2, 3
	 * and 4 and the player-start line.
	 */
	static constexpr float LanePylonXFrac = 0.3500f;   // 4200
	static constexpr float LanePylonYFrac = 0.5667f;   // 3400
	static constexpr float WingPylonSide = 220.f;
	static constexpr float WingPylonHeight = 1300.f;

	/** The four pylons standing around the centre dais. */
	static constexpr float DaisPylonXFrac = 0.2167f;   // 2600
	static constexpr float DaisPylonYFrac = 0.2667f;   // 1600
	static constexpr float DaisPylonSide = 240.f;
	static constexpr float DaisPylonHeight = 1800.f;

	/** Glowing ring on the floor marking the contested zone. Diameter as a fraction of HalfWidth. */
	static constexpr float CentrePadDiameterFrac = 0.8667f;   // 5200
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
	// EVERY X POSITION BELOW WAS CHECKED against the wing platform diamonds, the outer cover blocks,
	// the gate towers and the endzone. The tight one is the wing: its tier 0 is a 2400 uu square
	// rotated 45 degrees about (6400, 4200), so it reaches Y = 5897 at exactly X = 6400 and shrinks
	// by 1 uu of Y per uu of X either side. Buttresses at 6000 and 7800 clear it by 300 and 1300 uu
	// respectively; one at 6400 would have been inside it.

	struct FButtressSpec
	{
		float PositionFrac;   // of HalfLength for the side walls, of HalfWidth for the end walls
		float Height;
	};

	/** Side walls (constant Y). Mirrored into +/-X and +/-Y, so 0.f is built once per wall. */
	static const FButtressSpec SideButtresses[] =
	{
		{ 0.0000f, 1700.f },   // 0      - midfield marker, the tallest of the row
		{ 0.2000f, 1050.f },   // 2400
		{ 0.5000f, 1600.f },   // 6000
		{ 0.6500f, 1050.f },   // 7800
		{ 0.8500f, 1600.f }    // 10200  - behind the goal line, inside the endzone
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

	/** Outer-lane floor stripes, as fractions of HalfWidth. */
	static constexpr float LaneStripeYFracs[] = { 0.5500f, 0.8833f };   // 3300, 5300
	static constexpr float LaneStripeWidth = 72.f;

	/** Corner pylons, in the dead space behind each goal line. */
	static constexpr float CornerPylonXFrac = 0.8833f;   // 10600
	static constexpr float CornerPylonYFrac = 0.8500f;   // 5100
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
	static constexpr float LampXFracs[] = { 0.2000f, 0.5500f, 0.8800f };   // 2400, 6600, 10560
	static constexpr float LampYFracs[] = { 0.3600f, 0.8000f };            // 2160, 4800

	/**
	 * Lamp height. MEASURED: this started at 500 and the floor directly beneath a lamp came back as a
	 * blown white-hot blob - inverse-square falloff means 220 cd at 5 m is 8.8 lux, three times the
	 * key light, on a floor whose whole job is to be near black. Lifting the lamps to 9 m drops that
	 * peak to 2.7 lux (about the key) and flattens the gradient across the floor, while costing
	 * almost nothing at the ranges that matter, because at 20 m the extra 4 m of height is lost in
	 * the hypotenuse. Height is the cheap dial here; intensity is the expensive one.
	 */
	static constexpr float LampZ = 900.f;

	/** Endzone gate: two towers on the goal line plus a beam across the top. */
	static constexpr float GateTowerYFrac = 0.5000f;   // 3000
	static constexpr float GateTowerSide = 420.f;
	static constexpr float GateTowerHeight = 2300.f;
	static constexpr float GateBeamSize = 300.f;
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

	// The builder sits at the arena centre and the arena is now 24000 uu long, which is well past
	// the default net cull distance - but this actor has nothing to replicate beyond its own
	// existence, and AActor's relevancy fields changed accessor form during the 5.x line, so it is
	// left alone deliberately (build contract section 1). Clients build their own copy of the
	// geometry from BeginPlay; if the actor ever went irrelevant mid-match nothing would change,
	// because the geometry is local and already built.

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
	bArenaBuilt = true;

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

	if (bBuildInteriorLayout)
	{
		BuildCentreDais(bBuildVisuals);
		BuildWingPlatforms(bBuildVisuals);
		BuildLaneRails(bBuildVisuals);
		BuildCoverField(bBuildVisuals);
	}

	if (bBuildFlankStructures)
	{
		BuildFlanks(bBuildVisuals);
	}

	BuildEndzones(bBuildVisuals);

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

	UE_LOG(LogTraceGame, Log, TEXT("Arena built (%.0f x %.0f x %.0f uu, visuals=%s, authority=%s): %d components."),
		FieldLength, FieldWidth, WallHeight,
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
		AddCollisionBlock(Wall.Center, Wall.Size, Wall.Name);

		if (bBuildVisuals)
		{
			AddMeshBlock(CubeMesh, Wall.Center, Wall.Size, WallMID, /*bCastShadow=*/true, Wall.Name);
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
		AddPawnStandoff(
			FVector(XSign * (HalfX - TraceArenaConstants::WallNeonStandoff * 0.5f), 0.f, StandoffZ),
			FVector(TraceArenaConstants::WallNeonStandoff, FieldWidth, WallHeight),
			TEXT("WallStandoffX"));
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

		const int32 RibsPerHalf = FMath::Clamp(FMath::FloorToInt(HalfX / TraceArenaConstants::WallRibSpacing), 0, 8);
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

		AddMeshBlock(CubeMesh, FVector(Sign * (HalfX - TrimInset), 0.f, TrimZ),
			FVector(TraceArenaConstants::WallTrimSize, FieldWidth, TraceArenaConstants::WallTrimSize),
			TeamTrimMID, false, TEXT("EndTrim"));

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
	const int32 HalfLinesY = FMath::Min(FMath::FloorToInt(HalfY / GridSpacing), MaxHalfLines);

	// Lines of constant X, running the full width. Each lies entirely in one half, so it takes that
	// half's colour outright.
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
			FVector(bCenter ? GridStripWidth * 3.f : GridStripWidth, FieldWidth, TraceArenaConstants::GridThickness),
			bCenter ? CenterMID : HalfColorMID(X),
			/*bCastShadow=*/false,
			TEXT("GridX"));
	}

	// Lines of constant Y run the whole length and therefore cross both halves, so each one is built
	// as two half-length strips rather than one - that is what keeps the territory read consistent.
	for (int32 Index = -HalfLinesY; Index <= HalfLinesY; ++Index)
	{
		const float Y = Index * GridSpacing;
		if (FMath::IsNearlyEqual(FMath::Abs(Y), HalfY, 1.f))
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

	// The dais itself: four 40 uu steps, rotated 45 degrees so it reads as a diamond and so a player
	// running at it from any of the eight compass directions meets a flat face square-on rather than
	// a corner. It is walkable from every side (see AddSteppedPlatform), which is what keeps the
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
		// THE PEDESTAL MUST NOT BLOCK PAWNS. It sits exactly where the Core lives, so blocking pawns
		// makes the objective unreachable by ANYONE:
		//
		//     pedestal collision half-extent  = 300 * 0.94 / 2 = 141 uu
		//   + character capsule radius        =                   34 uu
		//   = closest a player can stand      =                  175 uu from the centre
		//     UTraceSettings::PickupRadius    =                  110 uu
		//
		// 175 > 110, so the Core's pickup sphere could never overlap a capsule while the Core is at
		// home. That was measured, not reasoned about: ten bots walked to the centre and orbited at
		// exactly X = +/-175.03 with zero "Core picked up" events in the log. The pedestal is also
		// 60 uu tall against a 45 uu MaxStepHeight, so players cannot climb it either.
		//
		// Ignoring ECC_Pawn keeps everything the block was actually for — the Core's projectile sweep
		// still rests on it, ECC_Visibility still blocks hitscan and bot line-of-sight, ECC_Camera
		// still blocks the spring arm — and only removes the one response that made the objective
		// impossible to take. Waist-high scenery you can walk through beats an unwinnable match.
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
	// they are what makes the centre findable from the far end of a 24000 uu field.
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

void ATraceArenaBuilder::BuildWingPlatforms(bool bBuildVisuals)
{
	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();

	UMaterialInstanceDynamic* BodyMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.f,
			TraceArenaConstants::NeonNeutral, 0.026f)
		: nullptr;
	UMaterialInstanceDynamic* PylonBodyMID = BodyMID;

	// One platform per quadrant, each wearing the colour of the team whose half it stands in.
	for (const float XSign : { -1.f, 1.f })
	{
		const ETraceTeam HalfTeam = (XSign < 0.f) ? ETraceTeam::Blue : ETraceTeam::Orange;
		UMaterialInstanceDynamic* NeonMID = bBuildVisuals
			? MakeNeonMID(TraceTeamColor(HalfTeam), TraceArenaConstants::GlowLip)
			: nullptr;
		UMaterialInstanceDynamic* PylonNeonMID = bBuildVisuals
			? MakeNeonMID(TraceTeamColor(HalfTeam), TraceArenaConstants::GlowPylon)
			: nullptr;

		for (const float YSign : { -1.f, 1.f })
		{
			const FVector2D Centre(XSign * HalfX * TraceArenaConstants::WingXFrac,
				YSign * HalfY * TraceArenaConstants::WingYFrac);

			AddSteppedPlatform(Centre,
				TraceArenaConstants::WingTopTierSide, TraceArenaConstants::WingTierSideStep,
				TraceArenaConstants::WingTiers, 45.f, BodyMID, NeonMID, TEXT("Wing"));

			if (bBuildVisuals)
			{
				// Stands beside the platform, not on it - see LanePylonXFrac.
				AddPylon(FVector2D(XSign * HalfX * TraceArenaConstants::LanePylonXFrac,
						YSign * HalfY * TraceArenaConstants::LanePylonYFrac),
					TraceArenaConstants::WingPylonSide, TraceArenaConstants::WingPylonHeight,
					PylonBodyMID, PylonNeonMID, TEXT("LanePylon"));
			}
		}
	}
}

void ATraceArenaBuilder::BuildLaneRails(bool bBuildVisuals)
{
	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();

	UMaterialInstanceDynamic* BodyMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.f,
			TraceArenaConstants::NeonNeutral, 0.026f)
		: nullptr;

	// Waist-high segmented rails. They are what turns an open rectangle into a central spine plus two
	// lanes per side: long enough to break a sightline and to make a carrier commit to a route, with
	// wide gaps between segments so nothing is ever a corridor. The gaps matter for more than feel -
	// Trace's bots have no navmesh and steer straight at their target (ATraceBotController), so a
	// continuous rail would be something they grind along until the stuck-evade kicks in.
	struct FRailRow
	{
		float YFrac;
		const float* XFracs;
		int32 XCount;
	};

	const FRailRow Rows[] =
	{
		{ TraceArenaConstants::RailRowAYFrac, TraceArenaConstants::RailRowAXFracs, UE_ARRAY_COUNT(TraceArenaConstants::RailRowAXFracs) },
		{ TraceArenaConstants::RailRowBYFrac, TraceArenaConstants::RailRowBXFracs, UE_ARRAY_COUNT(TraceArenaConstants::RailRowBXFracs) }
	};

	for (const FRailRow& Row : Rows)
	{
		for (const float YSign : { -1.f, 1.f })
		{
			for (const float XSign : { -1.f, 1.f })
			{
				const ETraceTeam HalfTeam = (XSign < 0.f) ? ETraceTeam::Blue : ETraceTeam::Orange;
				UMaterialInstanceDynamic* NeonMID = bBuildVisuals
					? MakeNeonMID(TraceTeamColor(HalfTeam), TraceArenaConstants::GlowLip)
					: nullptr;
				UMaterialInstanceDynamic* FaceMID = bBuildVisuals
					? MakeNeonMID(TraceTeamColor(HalfTeam), TraceArenaConstants::GlowFace)
					: nullptr;

				for (int32 Index = 0; Index < Row.XCount; ++Index)
				{
					const FVector Centre(
						XSign * HalfX * Row.XFracs[Index],
						YSign * HalfY * Row.YFrac,
						TraceArenaConstants::RailHeight * 0.5f);

					AddNeonBlock(Centre,
						FVector(TraceArenaConstants::RailLength, TraceArenaConstants::RailThickness, TraceArenaConstants::RailHeight),
						0.f, BodyMID, NeonMID, /*bCollide=*/true, TEXT("LaneRail"), FaceMID);
				}
			}
		}
	}
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

	for (const float XSign : { -1.f, 1.f })
	{
		const ETraceTeam HalfTeam = (XSign < 0.f) ? ETraceTeam::Blue : ETraceTeam::Orange;
		UMaterialInstanceDynamic* NeonMID = bBuildVisuals
			? MakeNeonMID(TraceTeamColor(HalfTeam), TraceArenaConstants::GlowLip)
			: nullptr;
		UMaterialInstanceDynamic* FaceMID = bBuildVisuals
			? MakeNeonMID(TraceTeamColor(HalfTeam), TraceArenaConstants::GlowFace)
			: nullptr;

		// Diamonds, not squares. A pawn (or a bot steering straight at a target behind it) that meets
		// a 45-degree face gets deflected around the block by the movement component's own wall
		// sliding, instead of standing still pushing into a flat face until stuck-detection fires.
		for (const TraceArenaConstants::FCoverSpec& Spec : TraceArenaConstants::CoverBlocks)
		{
			for (const float YSign : { -1.f, 1.f })
			{
				const FVector Centre(XSign * HalfX * Spec.XFrac, YSign * HalfY * Spec.YFrac, Spec.Height * 0.5f);
				AddNeonBlock(Centre, FVector(Spec.Side, Spec.Side, Spec.Height), 45.f,
					BodyMID, NeonMID, /*bCollide=*/true, TEXT("Cover"), FaceMID);
			}
		}

		// One long slab straddling the centreline of each half, so the shortest route from the dais
		// to a goal is never a clear straight line.
		const FVector AxisCentre(XSign * HalfX * TraceArenaConstants::AxisCoverXFrac, 0.f,
			TraceArenaConstants::AxisCoverSize.Z * 0.5f);
		AddNeonBlock(AxisCentre, TraceArenaConstants::AxisCoverSize, 0.f,
			BodyMID, NeonMID, /*bCollide=*/true, TEXT("AxisCover"), FaceMID);
	}
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
	// kind: the gate towers stop at |Y| = 3000 and the endzone is otherwise flat. A tall column in
	// each corner closes the room off and, incidentally, gives a defender something to fight around.
	//
	// Built BEFORE the visuals-only section on purpose: these block, and a dedicated server has to
	// build the same collision the clients are predicting against.
	for (const float XSign : { -1.f, 1.f })
	{
		for (const float YSign : { -1.f, 1.f })
		{
			AddPylon(FVector2D(XSign * HalfX * TraceArenaConstants::CornerPylonXFrac,
					YSign * HalfY * TraceArenaConstants::CornerPylonYFrac),
				TraceArenaConstants::CornerPylonSide, TraceArenaConstants::CornerPylonHeight,
				BodyMID, HalfNeon[HalfIndex(XSign)], TEXT("CornerPylon"));
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
	const float BridgeZ = TraceArenaConstants::WingPylonHeight - TraceArenaConstants::BridgeDrop;
	for (const float XSign : { -1.f, 1.f })
	{
		for (const float YSign : { -1.f, 1.f })
		{
			const float PylonY = YSign * HalfY * TraceArenaConstants::LanePylonYFrac;
			const float WallY = YSign * (HalfY - TraceArenaConstants::ButtressDepth);
			const float Span = FMath::Abs(WallY - PylonY);

			AddMeshBlock(CubeMesh,
				FVector(XSign * HalfX * TraceArenaConstants::LanePylonXFrac, (PylonY + WallY) * 0.5f, BridgeZ),
				FVector(TraceArenaConstants::BridgeSize, Span, TraceArenaConstants::BridgeSize),
				HalfBridge[HalfIndex(XSign)], /*bCastShadow=*/false, TEXT("LightBridge"));
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
	const float HalfY = HalfWidth();
	const float Depth = FMath::Clamp(EndzoneDepth, 100.f, HalfX);

	const ETraceTeam Teams[] = { ETraceTeam::Blue, ETraceTeam::Orange };
	for (const ETraceTeam Team : Teams)
	{
		const float Sign = TeamEndSign(Team);
		const float CenterX = Sign * (HalfX - Depth * 0.5f);
		const float GoalX = Sign * (HalfX - Depth);
		const FLinearColor TeamColor = TraceTeamColor(Team);

		if (bBuildVisuals)
		{
			// The patch is tinted with the colour of the team that DEFENDS this end. Their opponent
			// is the one who scores on it - see ATraceEndzone's class comment. It is a lit surface
			// with a faint emissive term rather than a neon block, so it reads as a floor that glows
			// rather than as a light panel, and the goal line in front of it stays the bright thing.
			UMaterialInstanceDynamic* PatchMID = MakeSurfaceMID(
				TraceArenaConstants::Dim(TeamColor, 0.020f), 0.20f, 0.f, TeamColor, 0.10f);
			RegisterSideMID(Sign, PatchMID, /*bNeon=*/false, /*Intensity=*/0.10f, /*BaseDim=*/0.020f);
			AddMeshBlock(CubeMesh,
				FVector(CenterX, 0.f, TraceArenaConstants::PatchZ),
				FVector(Depth, FieldWidth, TraceArenaConstants::PatchThickness),
				PatchMID, /*bCastShadow=*/false, TEXT("EndzonePatch"));

			UMaterialInstanceDynamic* LineMID = MakeNeonMID(TeamColor, TraceArenaConstants::GlowGoalLine);
			RegisterSideMID(Sign, LineMID, /*bNeon=*/true, TraceArenaConstants::GlowGoalLine);
			AddMeshBlock(CubeMesh,
				FVector(GoalX, 0.f, TraceArenaConstants::GoalLineZ),
				FVector(TraceArenaConstants::GoalLineWidth, FieldWidth, TraceArenaConstants::GoalLineThickness),
				LineMID, /*bCastShadow=*/false, TEXT("GoalLine"));

			// Two rails on the floor running from the goal line back to the end wall, closing the
			// endzone off visually so it reads as a room you score into rather than as more floor.
			for (const float YSign : { -1.f, 1.f })
			{
				AddMeshBlock(CubeMesh,
					FVector(CenterX, YSign * (HalfY - TraceArenaConstants::GoalLineWidth * 0.5f), TraceArenaConstants::GoalLineZ),
					FVector(Depth, TraceArenaConstants::GoalLineWidth, TraceArenaConstants::GoalLineThickness),
					LineMID, /*bCastShadow=*/false, TEXT("EndzoneEdge"));
			}

			// --- Gate ----------------------------------------------------------------------------
			//
			// Two towers on the goal line with a beam across the top. This is the single biggest
			// piece of "you are attacking THAT way" signage in the arena: it is 2600 uu tall, lit in
			// the defending team's colour, and visible from the opposite endzone.
			UMaterialInstanceDynamic* GateBodyMID = MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.f,
				TeamColor, 0.026f);
			UMaterialInstanceDynamic* GateNeonMID = MakeNeonMID(TeamColor, TraceArenaConstants::GlowPylon);
			RegisterSideMID(Sign, GateBodyMID, /*bNeon=*/false, /*Intensity=*/0.026f, /*BaseDim=*/-1.f);
			RegisterSideMID(Sign, GateNeonMID, /*bNeon=*/true, TraceArenaConstants::GlowPylon);

			const float TowerY = HalfY * TraceArenaConstants::GateTowerYFrac;
			for (const float YSign : { -1.f, 1.f })
			{
				AddPylon(FVector2D(GoalX, YSign * TowerY),
					TraceArenaConstants::GateTowerSide, TraceArenaConstants::GateTowerHeight,
					GateBodyMID, GateNeonMID, TEXT("GateTower"));
			}

			const float BeamZ = TraceArenaConstants::GateTowerHeight + TraceArenaConstants::GateBeamSize * 0.5f;
			UMaterialInstanceDynamic* GateFaceMID = MakeNeonMID(TeamColor, TraceArenaConstants::GlowFace);
			RegisterSideMID(Sign, GateFaceMID, /*bNeon=*/true, TraceArenaConstants::GlowFace);
			AddNeonBlock(FVector(GoalX, 0.f, BeamZ),
				FVector(TraceArenaConstants::GateBeamSize, TowerY * 2.f + TraceArenaConstants::GateTowerSide, TraceArenaConstants::GateBeamSize),
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

		ATraceEndzone* Zone = World->SpawnActorDeferred<ATraceEndzone>(
			ATraceEndzone::StaticClass(), ZoneTransform, /*Owner=*/nullptr, /*Instigator=*/nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

		if (Zone == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("ATraceArenaBuilder: failed to spawn the %s endzone."), *TraceTeamName(Team).ToString());
			continue;
		}

		Zone->ConfigureZone(Team, FVector(Depth * 0.5f, HalfY, WallHeight * 0.5f));
		Zone->FinishSpawning(ZoneTransform);

		// We are very often spawned from ATraceGameMode::BeginPlay, i.e. from inside the world's
		// own begin-play sweep, and an actor created during that sweep is not guaranteed to be
		// dispatched by it. The endzone does all of its wiring in BeginPlay, so force it here -
		// DispatchBeginPlay self-guards, and the world's later pass will simply skip it.
		if (!Zone->HasActorBegunPlay())
		{
			Zone->DispatchBeginPlay();
		}

		SpawnedActors.Add(Zone);
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
		// Depth is clamped exactly as BuildEndzones clamps it, so the pads cannot end up inside (or
		// on the far side of) the endzone they are supposed to sit in front of. The lower bound is
		// the dais rather than 0: with a large EndzoneDepth the old Max(0.f, ...) put BOTH teams'
		// spawn lines at X = 0, i.e. on top of the centre dais — silently reproducing the exact
		// "spawned inside geometry" failure this build was shipped with.
		//
		// The resulting line (x = +/-8400 on the shipped field) was checked against every cover block
		// and wing platform in TraceArenaConstants: nothing is built across it. If you move the
		// interior layout, re-check it - a start pad inside a solid block is a pawn that spawns
		// embedded in the world.
		const float Depth = FMath::Clamp(EndzoneDepth, 100.f, HalfX);
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

UStaticMeshComponent* ATraceArenaBuilder::AddMeshBlock(UStaticMesh* Mesh, const FVector& LocalCenter, const FVector& Size,
	UMaterialInstanceDynamic* MID, bool bCastShadow, const TCHAR* DebugName, float YawDegrees)
{
	// Null-checked per the asset rules: a missing engine shape must degrade to "invisible", never
	// to a crash. Collision lives in separate box components, so the arena still plays.
	if (Mesh == nullptr || Root == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
		this, MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), FName(DebugName)));
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
	Component->SetRelativeRotation(FRotator(0.f, YawDegrees, 0.f));
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
	if (Root == nullptr)
	{
		return nullptr;
	}

	UBoxComponent* Component = NewObject<UBoxComponent>(
		this, MakeUniqueObjectName(this, UBoxComponent::StaticClass(), FName(DebugName)));
	if (Component == nullptr)
	{
		return nullptr;
	}

	Component->SetMobility(EComponentMobility::Movable);
	Component->SetupAttachment(Root);
	Component->SetRelativeLocation(LocalCenter);
	Component->SetRelativeRotation(FRotator(0.f, YawDegrees, 0.f));
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
		this, MakeUniqueObjectName(this, UBoxComponent::StaticClass(), FName(DebugName)));
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
	UMaterialInstanceDynamic* FaceNeonMID, bool bVerticalTrim)
{
	// Whether this block gets face trim is decided from GEOMETRY, not from whether the materials
	// resolved, because a dedicated server passes null MIDs and still has to build byte-identical
	// collision to the clients predicting against it.
	const bool bHasFaceTrim = bVerticalTrim && (Size.Z >= TraceArenaConstants::FaceTrimMinHeight);

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

	if (NeonMID == nullptr || Size.Z < TraceArenaConstants::FaceTrimMinHeight)
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
		this, MakeUniqueObjectName(this, UPointLightComponent::StaticClass(), FName(DebugName)));
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
