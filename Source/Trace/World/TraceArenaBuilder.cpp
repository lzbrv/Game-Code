// Copyright (c) Trace. All Rights Reserved.

#include "World/TraceArenaBuilder.h"

#include "Trace.h"
#include "TraceTypes.h"
#include "TraceSettings.h"              // GoalWidthFieldFraction / GoalHeightUU (spec v4 section 7, resized v5 section 4)
#include "Core/TraceCharacter.h"        // PlayerHeightUU() reads the capsule off the CDO
#include "Core/TraceGameState.h"        // GetScoringModeFor - the one authority on which mode runs
#include "Gameplay/TraceCore.h"        // Trace.Arena.GoalSides grants the Core and reads the tally
#include "Gameplay/TraceEndzone.h"
#include "World/TraceCoreSpawn.h"       // spec v17 §2 - the placed Core spawn marker
#include "World/TraceTeamPlayerStart.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/InstancedStaticMeshComponent.h"   // spec v7 §8 - the pooled arena geometry
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"   // USkyAtmosphereComponent AND ASkyAtmosphere
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"                      // GEngine (Trace.Arena.Perf)
#include "Engine/EngineTypes.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/GameViewportClient.h"          // GetStatUnitData() (Trace.Arena.Perf)
#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"                       // FPostProcessSettings
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"                   // Trace.Arena.CoveWalk steers the local pawn
#include "GameFramework/CharacterMovementComponent.h"   // Trace.Arena.WallStick reads wish vs actual velocity
#include "GameFramework/PlayerController.h"             // Trace.Arena.CoveWalk / Trace.Arena.Pose
#include "EngineUtils.h"                       // TActorIterator
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Scalability.h"                       // spec v11 §3 - the quality levels the fidelity ladder reads
#include "Settings/TraceGameUserSettings.h"    // Trace.Video.PresetAB drives the real settings path
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"            // NewObject, MakeUniqueObjectName

// --- The measurement harness for spec v7 §8. See the Trace.Arena.Perf block at the end of the file.
#include "Containers/Ticker.h"                 // FTSTicker
#include "HAL/IConsoleManager.h"               // FAutoConsoleVariableRef / FAutoConsoleCommand
#include "Misc/CommandLine.h"                  // -TraceArenaLegacyGeometry
#include "Misc/Parse.h"                        // FParse::Param
// DELIBERATELY NOT RHIStats.h. The obvious way to report draw calls is GNumDrawCallsRHI, and it was
// tried: it reads a flat ZERO every frame on this Metal build, because the counter is only published
// by the end-of-pipe GPU profiler's ProcessFrame and that does not run here. A column of zeroes in a
// perf table is worse than no column - it looks like a result. The draw-call claim in the report is
// therefore made from the primitive census (which IS directly observable, and which is what decides
// the draw count for a scene of unique-material blocks), not from a counter that does not work.
#include "UnrealClient.h"                      // FStatUnitData
#include "UObject/UObjectIterator.h"           // TObjectIterator (scene primitive census)

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

	/**
	 * Where in the POCKET the arena's own spawn fan sits, as an alpha along GetSpawnLineX()'s band
	 * (0 = just behind the foot of the back approach ramp, 1 = just short of the end wall).
	 *
	 * SPEC v28 §8 REPLACED StartInsetFraction WITH THIS, and it is not a rename. The old constant was
	 * 0.10 of the HALF LENGTH, subtracted from the goal line, i.e. the spawn line sat in front of the
	 * goal and moved with the length of the field. Spawns are behind the goal now, and what they have
	 * to stay clear of back there is the back ramp and the wall - neither of which is a fraction of
	 * the field length. An alpha along a band whose two ends are both derived is the form that keeps
	 * the pads correct when the goal, the pocket, the ramp or the capsule is retuned.
	 *
	 * 0.35 leans the fan toward the goal rather than the wall: a spawning player wants to be looking
	 * at the pitch through their own hoop, not at 1200 uu of end wall dressing, and ATraceGameMode's
	 * deeper respawn pads (alpha 0.85) are the ones that want the wall at their back.
	 */
	static constexpr float StartPocketAlpha = 0.35f;

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

	// --- The cove at the base of every wall (spec v9 section 10) -----------------------------------
	//
	// THE SKETCH: a vertical side wall curving smoothly into the floor. Built from the same nested
	// axis-aligned boxes as the corner banks, laid on a quarter ellipse instead of on a straight
	// line, and governed by exactly three numbers. Every one of them is a GAMEPLAY constraint, not
	// an art one, which is why they live here rather than in the material block below.

	/**
	 * Minimum tread on any cove step.
	 *
	 * THE MANTLE'S TOP PROBE IS WHAT SETS THIS. TryBeginMantle finds a vertical face ahead, then
	 * drops a trace CapsuleRadius * 0.5 = 17 uu PAST that face to find the top of the ledge. On a
	 * staircase with a tread narrower than 17 uu that probe lands on the step ABOVE the one it
	 * should have found, so it reports a ledge two risers up - 74 uu, which is inside the mantle
	 * window [55, 230] - and a cove step becomes a climbable ledge. At 24 uu the probe always lands
	 * on the adjacent tread and reports ONE riser (37 uu), which is below MantleMinHeightUU and is
	 * refused. 40% of margin on a 17 uu threshold, and the check is in the profile generator so a
	 * future depth/height change cannot quietly cross it.
	 *
	 * It is also what caps the slope: riser / tread = 37 / 24, i.e. the curve blends until it is
	 * about 57 degrees and the wall goes vertical from there. A stepped fillet cannot be tangent to
	 * the wall and still have treads.
	 *
	 * SPEC v10 SECTION 9 DEMOTED THIS TO A DERIVATION. It is no longer passed to the profile
	 * generator, because an ABSOLUTE minimum tread is only the right rule while there is exactly one
	 * riser height in the arena. v10 builds the cove at TWO resolutions (see CoveCollisionRiser and
	 * CoveVisualRiser), and a 24 uu floor applied to a 5 uu riser would have truncated the visual
	 * curve at a 12-degree slope - i.e. deleted the entire cove. What the mantle actually constrains
	 * is the SLOPE (rise per unit run over the probe's 17 uu reach), and slope is what FilletMaxSlope
	 * below now carries. This constant survives as the number that DEFINES that slope at the
	 * historical riser, so the shipped envelope is provably unchanged.
	 */
	static constexpr float FilletMinTread = 24.f;

	/**
	 * Steepest a cove is ever allowed to get, as rise / run. SPEC v10 SECTION 9.
	 *
	 * StepRise / FilletMinTread = 40 / 24 = 1.667, i.e. exactly the slope the v9 pair implied, which
	 * is the point: expressing the stop rule as a slope rather than as a tread makes it INDEPENDENT
	 * of how finely the curve is sampled, so the same envelope comes out at a 40 uu riser and at a
	 * 5 uu one. On the shipped geometry the inner-edge rule (FilletMinInner) stops both the bank and
	 * the wall cove first - measured slope at truncation is 1.00 on the banks and 1.28 on the wall
	 * fillets, both well under this - so this is a guard rail, not the active limit, and the v9
	 * envelope is reproduced to the millimetre.
	 *
	 * The mantle safety argument, restated in slope terms because that is the form that generalises:
	 * the top probe reaches 17 uu past the face it found, so the worst ledge it can report is
	 * 17 * slope. At 1.667 that is 28 uu, against MantleMinHeightUU = 55. At the slope the shipped
	 * cove actually truncates at (1.28) it is 22 uu. Either way no cove step can present itself as a
	 * mantleable ledge, and that no longer depends on the riser.
	 */
	static constexpr float FilletMaxSlope = StepRise / FilletMinTread;

	// --- Cove sampling: spec v10 section 9, "curved, not terraced" ---------------------------------
	//
	// v9 shipped ONE staircase doing two jobs at once - it was both the collision the pawn walks on
	// and the surface the player looks at - so its riser had to satisfy the walkable ceiling
	// (StepRise) and that is a 39 uu step you can plainly see. The user looked at it and said so.
	//
	// v10 splits the two jobs, because they have completely different cost functions:
	//
	//   COLLISION is a UBoxComponent per step and every one of those is a REGISTERED PRIMITIVE, the
	//   budget spec v10 section 9 explicitly guards (365 today against a 349 pre-cove baseline). It
	//   is also the thing every verified property of Demo 9 is about - walkable, no false mantle, no
	//   bot traps - so it is the half that must not be disturbed casually.
	//
	//   VISUAL is an INSTANCE in a pooled ISM, which costs no registered primitive at all (see
	//   AddInstancedBlock). Sampling the same curve eight times as finely is very nearly free.
	//
	// So the collision riser is halved (a strict improvement on every Demo 9 property: a smaller
	// riser is more walkable and less mantleable, and the envelope is identical because the stop
	// rules are unchanged) and the VISIBLE riser is dropped to 5 uu, which is 2.8% of a player's
	// height and is not resolvable at any distance a player is ever at.
	//
	// WHAT THE SPLIT COSTS, stated up front because it is the honest downside: the pawn stands on
	// the collision staircase, which circumscribes the ideal curve, while the eye sees the visual
	// staircase, which also circumscribes it but eight times more tightly. The foot can therefore
	// sit up to (CoveCollisionRiser - CoveVisualRiser) = 15 uu ABOVE the drawn surface and at most
	// CoveVisualRiser = 5 uu below it. 15 uu is 8.5% of a player's height, it only reaches that
	// maximum in the few uu either side of a collision riser, and it is always a hover rather than a
	// sink (feet clipping INTO the ground reads as a bug; standing a shoe's thickness proud of a
	// curve does not). Making it zero means giving collision the 5 uu riser too, which is 350-odd
	// extra box components - the budget this section forbids.
	static constexpr float CoveCollisionRiser = StepRise * 0.5f;

	/** Riser of the DRAWN cove. Instanced, so the only cost is instances; see the block above. */
	static constexpr float CoveVisualRiser = 5.f;

	/**
	 * Vertical spacing of the glowing contour lines on a cove.
	 *
	 * v9 put one on EVERY terrace, which was correct when there were seven of them and wrong the
	 * moment the shape became smooth: 55 evenly spaced glowing lines up a curve IS a terraced look,
	 * whatever the geometry underneath is doing. At 110 uu a bank carries three and a wall fillet
	 * two, which reads as deliberate striping on a curved surface rather than as edges of steps -
	 * and the banks still get the "which half am I on" team-colour read the v9 comment was defending.
	 */
	static constexpr float CoveContourSpacing = 110.f;

	/**
	 * A cove contour is INLAID, not a lip.
	 *
	 * AddNeonBlock's lip protrudes LipOut = 12 uu and stands LipHeight = 18 uu tall, which is a
	 * ledge - fine on a 39 uu terrace whose job is to look like a terrace, and self-defeating on a
	 * surface that is trying not to. 1.5 uu of protrusion is enough to beat z-fighting and nothing
	 * like enough to read as a step.
	 */
	static constexpr float CoveContourOut = 1.5f;
	static constexpr float CoveContourHeight = 6.f;

	/**
	 * How far a drawn cove shell reaches down past the top of the shell outside it.
	 *
	 * The visual cove is built from thin nested SHELLS - each one covers only its own riser band and
	 * its own tread - rather than from solid boxes rising out of the floor the way the collision
	 * staircase does. With 55 of them per bank, solid boxes would mean 55 nested 1500 x 16000 uu
	 * slabs of overdraw and 55 full-size shadow casters for a shape 350 uu tall. The overlap exists
	 * so floating point can never open a hairline crack between two shells.
	 */
	static constexpr float CoveShellOverlap = 1.f;

	/**
	 * The cove stops when its inner edge gets this close to the wall face.
	 *
	 * WallNeonStandoff, deliberately: that pawn-only shell already forbids a body from pressing
	 * closer than 40 uu to a wall, so a tread inside it would be walkable geometry nobody can stand
	 * on - visible ground that rejects you, which is exactly the kind of thing that reads as a bug.
	 * Stopping here also keeps the top tread wide enough to stand on with the standoff pushing you
	 * outward (the innermost step ends 43.8 uu out on the shipped numbers).
	 */
	static constexpr float FilletMinInner = WallNeonStandoff;

	// FilletRampClearance (12 uu) lived here until spec v28 §8. It was the gap kept between the top
	// of an end-wall cove step and the mode-B carry-in ramp that used to climb the same 940 uu of
	// floor in front of each hoop. The hoop and its ramps moved to the goal line, 2400 uu clear of
	// the wall, so there is nothing above the end cove to keep clear of - see BuildWallFillets.

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
	// MEASURED IN DAIS WIDTHS, NOT IN HALF LENGTHS (spec v28 §8's standing rule: a value that
	// modifies a base is stored relative to THAT base).
	//
	// These four pylons ring the centre diamond. They were 0.1833 of the half length, then 0.1310 -
	// re-derived by hand each time the field was lengthened so that they kept landing on the 2200 uu
	// the diamond wants. That is the shape of a bug: the diamond's size has nothing to do with the
	// length of the field, so every future lengthening silently marches its ring outward until it
	// stops reading as a ring. v28 lengthened the field again and 0.1310 would have put them at 2515.
	//
	// The base of the diamond is DaisTopTierSide + DaisTierSideStep x (DaisTiers - 1) = 1900 uu
	// across, so these are that, times the ratios the tuned 2200/1100 came out at. Change the dais
	// and the ring follows it; change the field and nothing moves.
	static constexpr float DaisPylonXPerDaisSide = 1.1579f;   // 2200 / 1900
	static constexpr float DaisPylonYPerDaisSide = 0.5789f;   // 1100 / 1900
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

	/**
	 * Gap between the outer edge of the mode-B hoop and the plane the gate stands on (spec v28 §8).
	 *
	 * The gate used to stand ON the goal line, which was free while the goal was a hole in a wall
	 * 2400 uu further back. It is not free now - see the note in BuildEndzones - so the gate is set
	 * this far up the pitch from the ring's outer edge. Small on purpose: the gate is still meant to
	 * read as the frame around the goal, not as a separate landmark in the middle of the field.
	 */
	static constexpr float GateGoalClearance = 300.f;

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

	// --- MODE B, SPEC v6 §4.3 + SPEC v28 §8: the free-standing ring ---------------------------------
	//
	// The goal is a lit HOOP. v6 made it a hole through the back wall; v28 §8 moved the wall
	// ClampedEndzoneDepth() further back and left the hoop hanging on the goal line with playable
	// floor on both sides, so there is no hole and no square opening any longer - just the annulus,
	// floating.
	//
	// HOW THE CIRCLE IS MADE, since "a cylinder" is not something a box collider can be. An annulus
	// of RingSegments spokes closes a square of nothing down to a circle. Each spoke is a box whose
	// inner face is a chord at exactly the ring radius, so the union of their inner faces is a
	// regular polygon CIRCUMSCRIBING the scoring disc - the physical hole is never smaller than the
	// disc that scores, which is the direction the error has to lean: a hole a hair too small would
	// bounce Cores off a goal the rule says went in.
	//
	// GoalRingOuterScale is now what the annulus WANTS to be rather than what it is: a free-standing
	// ring has to clear the floor, so the band is clamped by GoalRingOuterRadius(). At 1.55 on the
	// shipped mouth it would reach 286 uu underground.
	static constexpr int32 GoalRingSegments = 16;
	static constexpr float GoalRingOuterScale = 1.55f;   // annulus outer radius / mouth radius, before the float clamp
	static constexpr float GoalRingSpokeOverlap = 1.12f; // tangential slop so adjacent spokes meet

	/** Radial depth and forward proudness of the glowing hoop laid on the annulus's inner rim. */
	static constexpr float GoalRingRimDepth = 64.f;
	static constexpr float GoalRingRimProud = 40.f;

	/**
	 * Glow for the hoop. The brightest thing in the arena on purpose: spec v6 asks to "make it
	 * visually obvious what and where the goal is", it hangs 33600 uu from the other hoop with
	 * near-black everything behind it, and it is the thing every throw in mode B is aimed at.
	 */
	static constexpr float GlowGoalRing = 7.5f;

	// The lit alcove behind the ring (GoalAlcoveDepth, 700 uu) went with the wall in spec v28 §8. It
	// existed so the hoop framed a room instead of the void outside the level; what it frames now is
	// the pocket, the end wall's own dressing and, most of the time, somebody's spawn.

	/** Thickness of the approach ramp slabs. */
	static constexpr float GoalRampThickness = 140.f;

	/** Headroom left between the top of the hoop and the top of the perimeter wall. */
	static constexpr float GoalRingWallHeadroom = 120.f;
}

// -------------------------------------------------------------------------------------------------
// SPEC v9 SECTION 10 - THE COVE PROFILE GENERATOR.
//
// "Curve the corners of the arena walls, so that the crosssection of the arena looks something along
// these lines" - the sketch's side wall sweeping into the floor. One generator, used by BOTH callers
// (the corner banks and the wall fillets), so the two curves are the same curve at different scales
// and cannot drift apart.
//
// THE SHAPE. A quarter ELLIPSE, horizontal semi-axis Depth and vertical semi-axis Height, concave
// toward the room. Writing the solid's width at a height z, measured in from the wall face:
//
//     W(z) = Depth * (1 - sqrt(1 - ((Height - z) / Height)^2))
//
// W(0) = Depth (the toe, where the curve lies flat on the floor) and W(Height) = 0 (the top, where it
// is tangent to the wall). Everything in between is the fillet.
//
//     |                                    ###|  <- tangent to the wall
//     |  wall                            ##   |
//     |                                ##     |     Height
//     |                             ###       |
//     |                       ######          |
//     |____________###########________________|  <- tangent to the floor
//                            <-- Depth -->
//
// THE STAIRCASE. Not a rotated ramp, for the reason recorded on the corner banks: Trace's bots have
// no navmesh and steer straight at their target with AddMovementInput, and a pitched collider met
// side-on is a wall to them. Nested axis-aligned boxes rising from Z = 0 are the same primitive the
// rest of the arena is made of, and UCharacterMovementComponent walks up anything under MaxStepHeight
// without the mover ever knowing there was a step. Step k is a box from the wall out to W(k * Riser)
// whose top is at (k + 1) * Riser, so its TREAD is the strip between its own outer edge and the next
// step's, and its RISER is exactly Riser.
//
// THE THREE STOP RULES, and each of them is a defect that was reasoned out rather than discovered:
//
//   RISER <= MaxRiser        Derived, never assumed: the step count is CEILed from Height / MaxRiser
//                            so raising the cove adds steps instead of making them taller. At
//                            StepRise (40, itself 5 under the engine's MaxStepHeight) a cove is
//                            walkable by construction - and 40 is also below MantleMinHeightUU (55),
//                            so no single riser can ever present itself as a climbable ledge.
//
//   SLOPE <= MaxSlope        The mantle's top probe lands CapsuleRadius * 0.5 = 17 uu past the face
//                            it found, so the tallest ledge it can ever report is 17 * slope. v9
//                            expressed this as an absolute minimum TREAD, which is the same rule
//                            only while the arena has exactly one riser height; spec v10 section 9
//                            samples this curve at two different risers, so it is stated as the
//                            slope it always was. See FilletMaxSlope for the numbers.
//
//   INNER EDGE >= MinInner   A cove is vertical at the top, so the last few steps of an untruncated
//                            one are slivers against the wall - inside the pawn standoff shell, i.e.
//                            walkable surfaces no body can reach. Stop before that.
//
// The last two are what truncate the curve short of tangency: the cove blends up to at most
// atan(MaxSlope) = 59 degrees, and on the shipped geometry the inner-edge rule bites first, at 45
// degrees on the banks and 52 on the wall fillets. The wall goes vertical from there. That is
// inherent to filling a concave corner with axis-aligned solids and it is why this is a blend rather
// than a true tangent join.
//
// SPEC v10 SECTION 9 - WHY THIS FUNCTION IS NOW CALLED TWICE PER RUN. The staircase above is a fine
// COLLIDER and a poor SURFACE, and v9 shipped one staircase doing both jobs, so the riser had to be
// walkable (<= StepRise) and the result was a visible 39 uu terrace. Callers now build the same
// curve twice - once at CoveCollisionRiser for the box colliders, once at CoveVisualRiser for the
// drawn shells - which is only sound because every stop rule above is now scale-free, so the two
// profiles describe the SAME envelope at two resolutions rather than two different shapes.
// -------------------------------------------------------------------------------------------------
namespace
{
	/** One box of a cove: how tall it is, and how far its outer edge stands off the wall face. */
	struct FTraceCoveStep
	{
		float TopZ = 0.f;
		float OuterDepth = 0.f;
	};

	/**
	 * Fills OutSteps with the staircase approximation of a quarter-ellipse cove, outermost/lowest
	 * first. Empty if the cove is degenerate. Never returns a step that breaks any of the three rules
	 * above, so a caller can build every step it is handed without re-checking anything.
	 */
	void TraceBuildCoveProfile(float Depth, float Height, float MaxRiser, float MinInner, float MaxSlope,
		TArray<FTraceCoveStep>& OutSteps)
	{
		OutSteps.Reset();

		if (Depth <= 1.f || Height <= 1.f || MaxRiser <= 1.f || MaxSlope <= 0.f)
		{
			return;
		}

		// CEIL, so the riser is always STRICTLY under the ceiling rather than equal to it - the same
		// rule the corner banks have always used, and for the same reason: a riser that lands exactly
		// on MaxStepHeight is one floating-point tie away from being an unclimbable wall.
		//
		// The 256 ceiling is a runaway guard, not a design limit. It used to be 32, which was one
		// more than the shipped cove needed and eight times fewer than spec v10 section 9's DRAWN
		// cove needs: at CoveVisualRiser the 352 uu banks want 71 samples. A clamp that silently
		// coarsened the curve back to 11 uu risers would have looked exactly like the fix working.
		const int32 StepCount = FMath::Clamp(FMath::CeilToInt(Height / MaxRiser), 1, 256);
		const float Riser = Height / static_cast<float>(StepCount);

		// The tread floor is DERIVED from the riser and the slope cap rather than given, so the shape
		// this produces depends only on Depth/Height/MinInner/MaxSlope and not on how finely it is
		// sampled. That is the whole reason spec v10 section 9 can draw the same cove at a 5 uu riser
		// that it collides at a 20 uu one and have the two agree on where the curve stops.
		const float MinTread = Riser / MaxSlope;

		float PreviousDepth = 0.f;

		for (int32 Index = 0; Index < StepCount; ++Index)
		{
			// Width of the ideal solid at the BOTTOM of this step, not the top. That is what makes the
			// staircase CIRCUMSCRIBE the ellipse instead of being cut inside it - with the top reading
			// the toe would start at W(Riser) and the flattest, widest part of the curve, which is the
			// part the eye actually reads as "it curves", would be missing.
			const float BottomZ = Riser * static_cast<float>(Index);
			const float Normalised = (Height - BottomZ) / Height;                       // 1 at the floor, 0 at the wall
			const float StepDepth = Depth * (1.f - FMath::Sqrt(FMath::Max(0.f, 1.f - Normalised * Normalised)));

			if (Index > 0 && (StepDepth < MinInner || (PreviousDepth - StepDepth) < MinTread))
			{
				break;   // And everything above it too: the steps have to stay monotone in both axes.
			}

			FTraceCoveStep& Step = OutSteps.AddDefaulted_GetRef();
			Step.TopZ = Riser * static_cast<float>(Index + 1);
			Step.OuterDepth = StepDepth;

			PreviousDepth = StepDepth;
		}
	}

	/**
	 * Marks which shells of a drawn cove carry a glowing contour line. SPEC v10 SECTION 9.
	 *
	 * Always the toe and always the crest - those two are the shape's silhouette against the floor
	 * and against the wall, and they are the lines that were already there in v9 - plus whichever
	 * shells the height sweeps past a Spacing boundary on. Spacing <= 0 marks every shell, which is
	 * what the pre-v9 A/B arm wants.
	 */
	void TraceMarkCoveContours(const TArray<FTraceCoveStep>& Steps, float Spacing, TArray<bool>& OutMarks)
	{
		OutMarks.Init(false, Steps.Num());

		if (Steps.Num() == 0)
		{
			return;
		}

		if (Spacing <= 0.f)
		{
			OutMarks.Init(true, Steps.Num());
			return;
		}

		OutMarks[0] = true;
		OutMarks[Steps.Num() - 1] = true;

		float NextContourZ = Spacing;
		for (int32 Index = 0; Index < Steps.Num(); ++Index)
		{
			if (Steps[Index].TopZ >= NextContourZ)
			{
				OutMarks[Index] = true;
				// Advance past this shell rather than by one spacing, so a coarse profile whose
				// risers exceed the spacing cannot mark every single step and quietly reinstate the
				// per-terrace lip this constant exists to remove.
				NextContourZ = Steps[Index].TopZ + Spacing;
			}
		}
	}
}

// -------------------------------------------------------------------------------------------------
// SPEC v7 §8 - THE A/B ARM.
//
// The whole reason this knob exists is the lesson from the last performance pass: a measurement with
// no BEFORE arm in the same binary is not a measurement. Demo 6's "the ribbon fixed it" was produced
// by comparing two capped runs, and it was wrong. So the pre-instancing geometry path is kept, and
// Trace.Arena.Perf can be run against either arm of the SAME build:
//
//   Trace.Arena.Instancing 1  (default) - one pooled ISM per (mesh, material, shadow) key.
//   Trace.Arena.Instancing 0            - the old path: one Movable UStaticMeshComponent per block.
//
// Read ONCE at the top of BuildArena into bBuildingInstancedGeometry, because the arena is built in a
// single pass and a knob that changed halfway through would produce a half-instanced arena that
// matches neither arm. To take the BEFORE measurement, launch with -TraceArenaLegacyGeometry: the
// arena is built long before any console command can run, so the command line is the only lever that
// is in place in time.
// -------------------------------------------------------------------------------------------------
namespace
{
	int32 GArenaInstancing = 1;
	FAutoConsoleVariableRef CVarArenaInstancing(
		TEXT("Trace.Arena.Instancing"),
		GArenaInstancing,
		TEXT("1 (default) = arena geometry is pooled into instanced static meshes by mesh+material+shadow. ")
		TEXT("0 = the pre-spec-v7 path, one Movable UStaticMeshComponent per block, kept only as the BEFORE ")
		TEXT("arm of Trace.Arena.Perf. Read once when the arena is built, so it must be set before then - ")
		TEXT("use the -TraceArenaLegacyGeometry command line switch."),
		ECVF_Default);

	/**
	 * SPEC v9 SECTION 10's A/B. Same shape as the instancing knob above, same reason: a fix with no
	 * BEFORE arm in the same binary is not a fix anybody can check. 0 rebuilds the arena the way it
	 * was - straight bank terraces, no cove - so Trace.Arena.CrossSection can be run against both.
	 */
	int32 GArenaWallCove = 1;
	FAutoConsoleVariableRef CVarArenaWallCove(
		TEXT("Trace.Arena.WallCove"),
		GArenaWallCove,
		TEXT("1 (default) = the corner banks are laid on a quarter ellipse and every wall base carries a ")
		TEXT("cove (spec v9 10). 0 = the pre-v9 shape, evenly spaced bank terraces and a square wall/floor ")
		TEXT("join, kept as the BEFORE arm. Read once when the arena is built - use -TraceArenaSquareCorners ")
		TEXT("to get it in place before the game mode builds the arena."),
		ECVF_Default);
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

	// STATIC, and this line is load-bearing for spec v7 §8.
	//
	// It used to be Movable, with a comment arguing that Static bought nothing because this project
	// disables static lighting. Lighting was never the point. A component may not be LESS mobile than
	// its attach parent, so a Movable root forces every one of the ~1450 pieces below it to be Movable
	// too - and Movable is what puts a primitive in the dynamic half of the renderer and the dynamic
	// half of the physics scene: excluded from cached shadow depths, re-evaluated for GPU-scene
	// transform upload, and held as a kinematic body in the physics broadphase rather than in the
	// static tree. The arena never moves. Not one piece of it.
	//
	// The reverse is legal and is used deliberately: the floor-lamp lattice below is Movable, and a
	// child MAY be more mobile than its parent. Nothing moves this actor - ATraceGameMode spawns it at
	// the origin and it stays there - and the editor is free to drag a level-placed one, because the
	// Static restriction is on moving a component in a GAME world, not on an editor transform.
	Root->SetMobility(EComponentMobility::Static);

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

	// THE TWO TRON MATERIALS, AND WHERE THEY LIVE NOW (spec v17 §3).
	//
	// These used to be resolved from /Game/Generated/Materials, which .gitignore excludes, under the
	// old policy that "the repo stays text-only and every developer regenerates them". That policy is
	// RETIRED: the arena bake committed a 639-file map and 66 material assets, and spec v17 §3 is
	// explicit that "nothing the game ships may depend on a gitignored generated asset". So the
	// COMMITTED parents at /Game/Trace/Materials/Parents are the source of truth and the only path
	// resolved here.
	//
	// A constructor-time FObjectFinder is what makes the cooker keep them; a bare runtime LoadObject
	// would resolve to null once cooked. That is also exactly why the /Game/Generated fallback is NOT
	// a finder - see ResolveArenaMaterials(). Cooking a reference to a gitignored directory is the
	// failure this section exists to prevent.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SurfaceFinder(TEXT("/Game/Trace/Materials/Parents/M_TraceSurface.M_TraceSurface"));
	if (SurfaceFinder.Succeeded())
	{
		SurfaceMaterial = SurfaceFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Trace/Materials/Parents/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		NeonMaterial = NeonFinder.Object;
	}
}

void ATraceArenaBuilder::ResolveArenaMaterials()
{
	// -----------------------------------------------------------------------------------------
	// SPEC v17 §0 RULE 1, FOR THE MATERIALS: "asset missing, asset fails to load, or toggle off =>
	// the game does exactly what it does today and SAYS SO in the log."
	//
	// Three arms, tried in order, and the one that won is logged every time so nobody has to guess
	// which materials a screenshot was taken with:
	//
	//   1. /Game/Trace/Materials/Parents/M_*        committed, cooked, the source of truth. Resolved
	//                                               in the constructor above.
	//   2. /Game/Generated/Materials/M_*            the LEGACY generator output. Gitignored, so it
	//                                               exists only on a machine that has run
	//                                               Scripts/generate_content.py pointed there. Loaded
	//                                               HERE rather than in the constructor precisely so
	//                                               the cooker never records a dependency on it.
	//   3. /Engine/BasicShapes/BasicShapeMaterial   flat and lit. The arena still plays; it just is
	//                                               not neon. MakeSurfaceMID/MakeNeonMID do this on
	//                                               their own when both of the above are null.
	//
	// Idempotent and cheap: once arm 1 or 2 has answered, both pointers are non-null and the whole
	// function is two branches. Called at the top of BuildArena and again from AdoptBakedArena, which
	// needs it for a completely different reason - a baked level's pieces already wear committed
	// instances, but the ONE log line saying which material set is live is worth having on both paths.
	if (SurfaceMaterial != nullptr && NeonMaterial != nullptr)
	{
		return;
	}

	auto LoadLegacy = [](const TCHAR* Path) -> UMaterialInterface*
	{
		return LoadObject<UMaterialInterface>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	};

	const bool bWantedSurface = (SurfaceMaterial == nullptr);
	const bool bWantedNeon = (NeonMaterial == nullptr);

	if (bWantedSurface)
	{
		SurfaceMaterial = LoadLegacy(TEXT("/Game/Generated/Materials/M_TraceSurface.M_TraceSurface"));
	}
	if (bWantedNeon)
	{
		NeonMaterial = LoadLegacy(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	}

	const bool bRecovered = (bWantedSurface && SurfaceMaterial != nullptr)
		|| (bWantedNeon && NeonMaterial != nullptr);

	if (bRecovered)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Arena] /Game/Trace/Materials/Parents is missing or failed to load. FELL BACK to the ")
			TEXT("legacy generator output at /Game/Generated/Materials (surface %s, neon %s). The arena ")
			TEXT("renders as it always did, but that directory is GITIGNORED - restore ")
			TEXT("Content/Trace/Materials from the repo, or re-author it with Scripts/generate_content.py."),
			(SurfaceMaterial != nullptr) ? TEXT("ok") : TEXT("MISSING"),
			(NeonMaterial != nullptr) ? TEXT("ok") : TEXT("MISSING"));
	}
}

// -------------------------------------------------------------------------------------------------
// Contract surface
// -------------------------------------------------------------------------------------------------

ATraceArenaBuilder* ATraceArenaBuilder::Get(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<ATraceArenaBuilder> It(const_cast<UWorld*>(World)); It; ++It)
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

ATraceCoreSpawn* ATraceArenaBuilder::FindPlacedCoreSpawn() const
{
	// Cached because ATraceCore::GetHomeLocation() asks this question on every kickoff, every reset
	// after a score and from a couple of debug paths, and it already pays one full actor iteration to
	// find this builder. The weak pointer is re-resolved if the marker is destroyed, so a designer
	// deleting it in the editor is not left with a stale answer.
	if (ATraceCoreSpawn* Cached = PlacedCoreSpawn.Get())
	{
		return Cached;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	ATraceCoreSpawn* Found = nullptr;
	int32 Count = 0;

	for (TActorIterator<ATraceCoreSpawn> It(World); It; ++It)
	{
		ATraceCoreSpawn* Marker = *It;
		if (!IsValid(Marker))
		{
			continue;
		}

		++Count;
		if (Found == nullptr)
		{
			Found = Marker;
		}
	}

	// Reported ONCE per resolve rather than per call: the first one wins so the arena is never left
	// with no Core spawn at all, but a second marker means half the answers in the level are wrong
	// and that is a very expensive thing to find by playing.
	if (Count > 1)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Arena] %d ATraceCoreSpawn actors in this level; there must be exactly one. Using '%s' ")
			TEXT("at %s and ignoring the rest. Delete the spares in the World Outliner (Arena/Spawns)."),
			Count, *Found->GetName(), *Found->GetActorLocation().ToCompactString());
	}

	PlacedCoreSpawn = Found;
	return Found;
}

FVector ATraceArenaBuilder::GetCoreSpawnLocation() const
{
	// --- SPEC v17 §2: THE PLACED MARKER WINS, AND IT IS OPT-IN ------------------------------------
	//
	// Spec v15 §1.4 asked for the Core spawn to be a real placed actor and it never became one. It is
	// one now: if the level contains an ATraceCoreSpawn, ITS transform is the answer, so a designer
	// who drags the centre dais can drag the Core spawn with it. See that class's header.
	//
	// No marker means exactly the old behaviour, derived from the layout properties below — which is
	// what /Game/Maps/Arena still does, and what any level baked before this pass does.
	if (const ATraceCoreSpawn* Marker = FindPlacedCoreSpawn())
	{
		return Marker->GetActorLocation();
	}

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
	//
	// SPEC v28 §8: this is also THE GOAL PLANE - the hoop hangs on it - and everything from here back
	// to the end wall is the playable pocket. One line, one meaning, in both scoring modes.
	return HalfLength() - ClampedEndzoneDepth();
}

float ATraceArenaBuilder::GetGoalPlaneX() const
{
	return GoalLineX();
}

FBox ATraceArenaBuilder::GetSpawnPocketBounds(float EndSign) const
{
	// Goal plane to end wall. Identical to GetEndzoneBounds by construction - see the header.
	return GetEndzoneBounds(EndSign);
}

float ATraceArenaBuilder::GetSpawnLineX(float EndSign, float Alpha) const
{
	const float Sign = (EndSign < 0.f) ? -1.f : 1.f;

	// NEAR EDGE: behind the foot of the back ramp, plus a capsule diameter so a pawn standing on the
	// pad is clear of the slope rather than balanced on its toe. The ramp starts at the HOOP'S FACE,
	// not at the goal plane, so its half thickness is part of the reach - miss that and alpha 0 lands
	// a few uu up the slope, which is exactly the sort of "nearly right" a spawn pad cannot be.
	const float NearX = GoalLineX() + GoalRingHalfThickness() + GoalRampRun()
		+ TraceArenaConstants::PawnCapsuleRadius * 2.f;

	// FAR EDGE: off the end wall by its pawn standoff plus a capsule radius plus a little, so a pad
	// is never inside the shell that holds bodies off the wall (which would spawn a pawn intersecting
	// invisible collision and let the engine push it somewhere unpredictable).
	const float FarX = HalfLength() - TraceArenaConstants::WallNeonStandoff
		- TraceArenaConstants::PawnCapsuleRadius - 60.f;

	// A pocket too shallow to hold a band at all: put everything in the middle of what there is and
	// say so. Better a crowded spawn line than one inside the ramp or inside the wall.
	if (FarX <= NearX)
	{
		return Sign * FMath::Max(0.f, (GoalLineX() + HalfLength()) * 0.5f);
	}

	return Sign * FMath::Lerp(NearX, FarX, FMath::Clamp(Alpha, 0.f, 1.f));
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

float ATraceArenaBuilder::GoalRingOuterRadius() const
{
	const float Radius = GoalRingRadius();

	// What the annulus WANTS to be: the same 1.55x band the ring has always been drawn with.
	const float WantedBand = FMath::Max(40.f, Radius * (TraceArenaConstants::GoalRingOuterScale - 1.f));

	// SPEC v28 §8. What it may be, now that there is no wall to bury the bottom half in: the hoop has
	// to hold itself off the floor by GoalRingFloatPlayerHeights. The band is measured DOWN from the
	// bottom of the mouth, which is GoalRingClearanceZ() off the deck, so the room available is that
	// clearance minus the float gap.
	const float FloatGap = FMath::Max(0.f, GoalRingFloatPlayerHeights) * PlayerHeightUU();
	const float RoomBelowMouth = FMath::Max(20.f, GoalRingClearanceZ() - FloatGap);

	return Radius + FMath::Min(WantedBand, RoomBelowMouth);
}

float ATraceArenaBuilder::GoalRingHalfThickness() const
{
	// The hoop keeps EXACTLY the depth it had while it was set into the end wall, so its silhouette
	// from the halfway line is the one that shipped. Relative to WallThickness rather than a fresh
	// constant for that reason - "as deep as the wall it used to live in" is the whole rule.
	return FMath::Max(20.f, WallThickness * 0.5f);
}

float ATraceArenaBuilder::GoalRingCentreZ() const
{
	return GoalRingClearanceZ() + GoalRingRadius();
}

float ATraceArenaBuilder::GoalSlabHalfDepth() const
{
	// The floor: a carrier at the top of a ramp is stopped by the hoop's own face, so their origin
	// can get no closer to the plane than half the ring's thickness plus their capsule radius. A slab
	// shallower than that could be pressed against and never entered - a goal that cannot be carried
	// into. 60 uu of margin on top so the case is comfortably inside rather than exactly on the edge.
	const float Reachable = GoalRingHalfThickness() + TraceArenaConstants::PawnCapsuleRadius + 60.f;

	// Never deeper than the pocket, or the slab would reach past the end wall at one end and into the
	// approach cover at the other.
	return FMath::Clamp(GoalRingDepth * 0.5f, Reachable, FMath::Max(Reachable, ClampedEndzoneDepth()));
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

float ATraceArenaBuilder::GoalRampRun() const
{
	const float RampTop = GoalRampTopZ();
	if (RampTop <= 1.f)
	{
		return 0.f;
	}

	// ONE function, because the ramp's run is now read in three places - the two ramps themselves and
	// the near edge of the spawn band, which must start behind the foot of the back one.
	return FMath::Max(200.f, RampTop * FMath::Max(0.5f, GoalRampRunPerRise));
}

FVector ATraceArenaBuilder::GetGoalRingCentre(float EndSign) const
{
	const float Sign = (EndSign < 0.f) ? -1.f : 1.f;

	// SPEC v28 §8: the GOAL PLANE, not the end wall. They were the same point until the wall moved
	// ClampedEndzoneDepth() further back to open the pocket behind the hoop.
	return GetActorTransform().TransformPosition(FVector(Sign * GoalLineX(), 0.f, GoalRingCentreZ()));
}

FBox ATraceArenaBuilder::GetGoalBounds(float EndSign) const
{
	const float Sign = (EndSign < 0.f) ? -1.f : 1.f;
	const float PlaneX = Sign * GoalLineX();
	const float Radius = GoalRingRadius();
	const float CentreZ = GoalRingCentreZ();
	const float HalfDepth = GoalSlabHalfDepth();

	// SPEC v6 §4.3 made the goal a shallow SLAB across the mouth of the ring, with the disc inscribed
	// in that slab as what actually scores (ATraceEndzone::ConfigureRing). SPEC v28 §8 CENTRES that
	// slab ON THE RING PLANE instead of hanging it off the field side of a wall, and that one change
	// is the whole of "allow goals to be scored through either side of the goal": the disc test and
	// the swept crossing test never cared which way the Core was travelling, but half the volume they
	// test inside used to be buried in masonry.
	//
	// It is what bot targeting and debug draw read, and it is deliberately conservative, so anything
	// that treats it as the goal aims at the middle of the hoop rather than past it.
	const FBox Local(
		FVector(PlaneX - HalfDepth, -Radius, CentreZ - Radius),
		FVector(PlaneX + HalfDepth, Radius, CentreZ + Radius));

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
	if (bArenaBuilt)
	{
		return;
	}

	// SPEC v15 §1.5 - "turn OFF the runtime build for a baked level", and the check lives here rather
	// than in ATraceGameMode because the game mode has no business knowing which levels are baked.
	// The geometry is already in the .umap; what is missing is the wiring, and that is what adopting
	// does. See bLevelIsPreBaked in the header for why this actor is still in the baked level at all.
	if (IsLevelPreBaked())
	{
		AdoptBakedArena();
		WarnIfHitscanRangeIsShort();
		return;
	}

	BuildArena();
	WarnIfHitscanRangeIsShort();
}

// =================================================================================================
// THE STANDING RULE, ENFORCED AT STARTUP RATHER THAN IN A COMMENT.
//
// UTraceSettings::HitscanRange is DERIVED from this actor's FieldLength and FieldWidth: it has to
// span the arena's diagonal or a shot down the long axis expires in mid-air short of a target the
// player can plainly see. It has now been left behind TWICE by a pass that lengthened the field -
// spec v4 §3 (24000 -> 33600, range left at 28000) and spec v28 §8 (33600 -> 38400, range left at
// 36000). BOTH TIMES THE PAIRING WAS WRITTEN DOWN IN A COMMENT NEXT TO THE VALUE, in this header and
// in Config/DefaultGame.ini, and both times the comment was not enough.
//
// So the check runs on EVERY startup, on both paths (the procedural build and the baked adopt),
// costs one sqrt, and prints at Error when it fails. Trace.Arena.VerifyHitscanReach measures the
// same thing far more thoroughly - it walks the real ray and finds the true sight line - but it only
// helps somebody who thinks to run it, and nobody who lengthens a field thinks to run a gun command.
// This one cannot be missed: it is in the log of every match anybody plays.
//
// IT WARNS, IT DOES NOT CLAMP. Silently raising a designer's number would make Config/DefaultGame.ini
// stop being the authority it is documented to be, and a knob that quietly disagrees with its own
// file is the failure this project already keeps a house rule about. The fix is one line in each of
// two files and the message says so.
// =================================================================================================
void ATraceArenaBuilder::WarnIfHitscanRangeIsShort() const
{
	const float DiagonalUU = FMath::Sqrt(FieldLength * FieldLength + FieldWidth * FieldWidth);
	const float RangeUU    = UTraceSettings::Get().HitscanRange;

	if (RangeUU >= DiagonalUU)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Arena] HitscanRange %.0f uu spans the %.0f x %.0f arena's %.0f uu diagonal (%.0f uu spare)."),
			RangeUU, FieldLength, FieldWidth, DiagonalUU, RangeUU - DiagonalUU);
		return;
	}

	UE_LOG(LogTraceGame, Error,
		TEXT("[Arena] *** THE GUN CANNOT CROSS THIS ARENA. *** UTraceSettings::HitscanRange is %.0f uu and the "
		     "%.0f x %.0f field's diagonal is %.0f uu, so a shot down the long axis dies %.0f uu SHORT of a "
		     "target the player can see, with nothing on screen saying why. HitscanRange is DERIVED from "
		     "FieldLength/FieldWidth and must move with them: set it to at least %.0f in BOTH "
		     "Source/Trace/TraceSettings.h AND Config/DefaultGame.ini (the ini wins). "
		     "Trace.Arena.VerifyHitscanReach measures it properly, against the real geometry."),
		RangeUU, FieldLength, FieldWidth, DiagonalUU, DiagonalUU - RangeUU,
		FMath::CeilToFloat(DiagonalUU / 100.f) * 100.f);
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

	// Unsubscribed on every EndPlay reason, not just Destroyed: the delegate is a global multicast
	// that outlives the world, and a handle left behind on level teardown is a call into a dead actor
	// the next time anybody touches the video settings. AddWeakLambda makes that safe rather than
	// fatal; removing it makes it not happen.
	if (ScalabilityChangedHandle.IsValid())
	{
		Scalability::OnScalabilitySettingsChanged.Remove(ScalabilityChangedHandle);
		ScalabilityChangedHandle.Reset();
	}

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

	// WHICH GEOMETRY PATH (spec v7 §8), read exactly once so the arena cannot come out half
	// instanced. The command-line switch wins because the arena is built from
	// ATraceGameMode::PreInitializeComponents, which is earlier than any console command can run -
	// there is no other way to get the BEFORE arm.
	bBuildingInstancedGeometry = (GArenaInstancing != 0)
		&& !FParse::Param(FCommandLine::Get(), TEXT("TraceArenaLegacyGeometry"));

	// WHICH WALL/FLOOR JOIN (spec v9 §10), latched here for exactly the reason above: the bank profile
	// and the cove have to agree with each other, and a knob that changed halfway through the build
	// would produce an arena whose sidelines curved and whose endzones did not.
	bBuildingSquareCorners = (GArenaWallCove == 0)
		|| FParse::Param(FCommandLine::Get(), TEXT("TraceArenaSquareCorners"));

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

	// Committed parents first, legacy generated output second, engine grey third - and it logs which
	// (spec v17 §0 rule 1). Called here rather than in the constructor because arm 2 must never become
	// a cooked dependency on a gitignored directory.
	ResolveArenaMaterials();

	if (bBuildVisuals && (SurfaceMaterial == nullptr || NeonMaterial == nullptr))
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("ATraceArenaBuilder: neither /Game/Trace/Materials/Parents nor the legacy ")
			TEXT("/Game/Generated/Materials could supply M_TraceSurface and M_TraceNeon, so the build falls ")
			TEXT("back to /Engine/BasicShapes/BasicShapeMaterial. The arena will render flat and lit instead ")
			TEXT("of neon, and it still plays. Restore Content/Trace/Materials from the repo (git lfs pull), ")
			TEXT("or re-author the parents with Scripts/generate_content.py."));
	}
	else if (bBuildVisuals)
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("[Arena] Materials: surface '%s', neon '%s'."),
			*GetPathNameSafe(SurfaceMaterial), *GetPathNameSafe(NeonMaterial));
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

	// Spec v9 section 10. AFTER the banks so its own log line reads in the order the cross-section
	// does, and before anything that stands on the ground - though the order does not actually matter
	// to the geometry: the cove is a union with whatever it crosses, not a replacement for it.
	if (bBuildWallFillets && !bBuildingSquareCorners)
	{
		BuildWallFillets(bBuildVisuals);
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

	// EVERY POOL GOES LIVE HERE, and the position of this line in the function is a correctness
	// constraint rather than a tidiness one. It has to be AFTER the last CollectPiecesSince (which is
	// what decides a pool's mobility) and BEFORE ApplyScoringMode (which rewrites instance transforms
	// through components that must already be registered at the right mobility).
	FlushInstancePools();

	// SPEC v11 §3. The volume and the lights exist now, so the quality ladder can be written into
	// them - and it has to be written HERE rather than inside BuildPostProcess/BuildLighting, because
	// it is re-applied on every later scalability change and there must be exactly one function that
	// decides what a tier means. Subscribing here rather than in BeginPlay covers the editor preview
	// and the measurement rebuild, both of which build without ever beginning play.
	if (!ScalabilityChangedHandle.IsValid())
	{
		ScalabilityChangedHandle = Scalability::OnScalabilitySettingsChanged.AddWeakLambda(this,
			[this](const Scalability::FQualityLevels&)
			{
				ApplyFidelity();
			});
	}
	ApplyFidelity();

	// Present one of the two scoring shapes and disarm the other. Forced rather than early-returned
	// on "the mode did not change", because at this point NOTHING has been presented yet: the goal
	// furniture is built visible and the endzone triggers are built armed, so mode A needs this call
	// exactly as much as mode B does.
	ApplyScoringMode(ScoringMode);

	// Spelled out at Log because the proportion, the flat playfield width and which game is being
	// played are the things a playtester asks about first, and none of them is readable from a
	// screenshot.
	// The primitive census, spelled out, because "how many things does this hand the renderer" is the
	// question spec v7 §8 is about and GetComponents().Num() alone answers it wrongly now: one pooled
	// ISM is one component and several hundred blocks.
	int32 Primitives = 0;
	int32 Movables = 0;
	for (const UActorComponent* Built : GetComponents())
	{
		if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Built))
		{
			++Primitives;
			Movables += (Primitive->Mobility == EComponentMobility::Movable) ? 1 : 0;
		}
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("Arena built (%.0f x %.0f x %.0f uu, %.2f:1, flat playfield %.0f wide, banks %s at %.0f uu, ")
		TEXT("mode %s, visuals=%s, authority=%s, geometry=%s): %d components, %d of them primitives ")
		TEXT("(%d Movable), carrying %d instanced blocks in %d pools."),
		FieldLength, FieldWidth, WallHeight,
		(FieldWidth > 0.f) ? (FieldLength / FieldWidth) : 0.f,
		BankInnerHalfWidth() * 2.f,
		bBuildCornerBanks ? TEXT("on") : TEXT("off"), BankHeight,
		*TraceScoringModeLabel(ScoringMode),
		bBuildVisuals ? TEXT("yes") : TEXT("no"),
		HasAuthority() ? TEXT("yes") : TEXT("no"),
		bBuildingInstancedGeometry ? TEXT("instanced") : TEXT("legacy one-component-per-block"),
		GetComponents().Num(), Primitives, Movables,
		BuiltInstances.Num(), InstancePools.Num());
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

	// NO WALL IS MODE-TAGGED ANY MORE (spec v28 §8), and deleting that tagging is a load-bearing part
	// of this pass rather than tidying. From spec v6 §4.3 until now, mode B needed a 2000 uu hole
	// through each end wall for the ring goal - which cannot be done by subtracting from a box - so
	// mode B built a perforated REPLACEMENT and this solid slab was presented only in mode A. The hoop
	// now floats on the goal line with ClampedEndzoneDepth() of playable pocket between it and the
	// wall, so the wall has no hole in it in either mode and there is exactly one end wall again.
	//
	// Leaving the tag in place would have been the worst kind of leftover: SetPiecesPresented would
	// have hidden and DE-COLLIDED both end walls for the whole of a mode B match, and the first thing
	// anybody did in the new pocket would have been to walk out of the world.
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
		// UNTAGGED AND PERMANENT since spec v28 §8, with the end wall it belongs to. It used to be
		// mode-tagged because a full-face pawn shell across an end wall would have held a mode-B
		// carrier 74 uu OFF the goal they were trying to run through. The goal is 2400 uu away from
		// this wall now; the shell it would have blocked no longer touches it, and the pocket behind
		// the hoop is exactly where a body most wants a wall it cannot press into.
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

		// The top strip: the line that reads the end of the field from midfield in both modes. It was
		// the only piece of end-wall dressing that was permanent while the wall had a hole in it; the
		// three below it join it now (spec v28 §8).
		AddMeshBlock(CubeMesh, FVector(Sign * (HalfX - TrimInset), 0.f, TrimZ),
			FVector(TraceArenaConstants::WallTrimSize, FieldWidth, TraceArenaConstants::WallTrimSize),
			TeamTrimMID, false, TEXT("EndTrim"));

		// THE BAND, THE KICK BAND AND THE RIBS ARE NOW PERMANENT TOO (spec v28 §8). They used to be
		// mode A only, because they crossed the height and the width the mode-B hoop occupied and
		// would have been three glowing bars drawn straight across the goal. The hoop hangs on the
		// goal line now, a whole pocket in front of this wall, and nothing on the wall is behind it -
		// so the end wall gets its full dressing in both modes, which is what the pocket needs: it is
		// the only structure a player standing at their own spawn is looking at.
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

	// The ring is measured off THE DAIS, which is what it rings - see the note on
	// DaisPylonXPerDaisSide. It used to be a fraction of the half length and of the half width, which
	// meant the diamond's own ring drifted outward every time the field was resized.
	const float DaisSide = TraceArenaConstants::DaisTopTierSide
		+ TraceArenaConstants::DaisTierSideStep * static_cast<float>(TraceArenaConstants::DaisTiers - 1);

	for (const float XSign : { -1.f, 1.f })
	{
		for (const float YSign : { -1.f, 1.f })
		{
			AddPylon(FVector2D(XSign * DaisSide * TraceArenaConstants::DaisPylonXPerDaisSide,
					YSign * DaisSide * TraceArenaConstants::DaisPylonYPerDaisSide),
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
	//    |Y| = 4800  ###############################____        <- crest, at the side wall
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
	// SPEC v9 SECTION 10 - THE CROSS-SECTION. The terraces used to be spaced EVENLY across the bank's
	// depth, which is a straight ramp: it met the floor at an angle at the toe and hit the side wall
	// dead square at the crest, and that square join at the wall is precisely what the sketch is
	// asking to be curved away. They are now laid on the quarter-ellipse TraceBuildCoveProfile
	// generates - flat where they meet the floor, steepening into the wall - so the bank IS the
	// fillet along the side walls rather than something a fillet has to be added to. The bowl, the
	// depth, the flat playfield width, the X taper and the goal-line setback are all unchanged;
	// only the Y position of each terrace moves.
	//
	// The crest lands lower than BankHeight and that is the truncation, not a bug: a cove is vertical
	// where it meets the wall, and the last steps of one are slivers inside the pawn standoff. See
	// TraceBuildCoveProfile. On the shipped 1500 x 352 bank the profile yields seven terraces topping
	// out at 274 uu with the crest tread 86 uu off the wall, against nine even ones topping out at
	// 352 uu. The wall cove (BuildWallFillets) picks the shape up from there.
	//
	// SPEC v10 SECTION 9 - CURVED, NOT TERRACED. v9's answer to "curve the corners" was to lay the
	// terraces on the ellipse, which fixed the PLAN of the shape and left its CROSS-SECTION a visible
	// flight of 39 uu stairs. The user looked at it and asked again. So the bank is now built twice
	// over the same envelope:
	//
	//   COLLISION - nested box components on the CoveCollisionRiser profile. Invisible. This is the
	//   surface every Demo 9 property is a statement about, and halving the riser can only improve
	//   all of them (more walkable, further below MantleMinHeightUU, same axis-aligned nested boxes
	//   the bots already handle). The envelope is unchanged to the millimetre because both stop rules
	//   are scale-free - see FilletMaxSlope.
	//
	//   VISUAL - thin nested shells on the CoveVisualRiser profile, one instance each in the pools
	//   the bank body already used, so the registered-primitive budget does not move for them at all.
	//   At 5 uu a riser is 2.8% of a player's height; there is no distance in this arena at which it
	//   resolves as a step.
	//
	// The X taper is now a CONTINUOUS function of height (BankAlpha below) instead of a per-terrace
	// index, which is what lets the two resolutions agree: a collision box and the shells that skin
	// it are cut to the same X span at the same Z, so no invisible collider ever sticks out past the
	// drawn surface along the field.
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

	// The cove, spec v9 section 10, sampled twice for spec v10 section 9 - COLLISION coarse enough
	// that the box-component count does not move, VISUAL fine enough that no riser is perceptible.
	// Same generator, same envelope, two resolutions; see TraceBuildCoveProfile.
	TArray<FTraceCoveStep> Terraces;        // colliders
	TArray<FTraceCoveStep> Skin;            // drawn shells
	if (bBuildingSquareCorners)
	{
		// THE BEFORE ARM. Evenly spaced terraces, i.e. a straight ramp - byte for byte the geometry
		// this function built before spec v9 §10, so Trace.Arena.CrossSection can measure the square
		// join this pass is meant to remove and show it failing first. The v10 split is deliberately
		// NOT applied here: the before arm has to stay the thing it was, terracing and all.
		for (int32 Index = 0; Index < TerraceCount; ++Index)
		{
			FTraceCoveStep& Terrace = Terraces.AddDefaulted_GetRef();
			Terrace.TopZ = Riser * static_cast<float>(Index + 1);
			Terrace.OuterDepth = Depth * (1.f - static_cast<float>(Index) / static_cast<float>(TerraceCount));
		}
		Skin = Terraces;
	}
	else
	{
		TraceBuildCoveProfile(Depth, Height, TraceArenaConstants::CoveCollisionRiser,
			TraceArenaConstants::FilletMinInner, TraceArenaConstants::FilletMaxSlope, Terraces);
		TraceBuildCoveProfile(Depth, Height, TraceArenaConstants::CoveVisualRiser,
			TraceArenaConstants::FilletMinInner, TraceArenaConstants::FilletMaxSlope, Skin);
	}

	if (Terraces.Num() == 0 || Skin.Num() == 0)
	{
		return;
	}

	// The goal line is where the bank has to stop. GoalLineX() rather than a hand-rolled
	// HalfX - EndzoneDepth, for the reason spelled out on ClampedEndzoneDepth(): three hand-written
	// copies of this clamp is how a spawn pad ended up on the wrong side of a goal line.
	const float GoalX = GoalLineX();
	const float Inboard = FMath::Max(0.f, GoalX * TraceArenaConstants::BankInboardTaperFrac);
	const float Setback = FMath::Min(TraceArenaConstants::BankGoalSetback, FMath::Max(0.f, GoalX * 0.5f));

	// THE X TAPER, AS A CONTINUOUS FUNCTION OF HEIGHT.
	//
	// v9 read it off the terrace INDEX (Alpha = k / TerraceCount), which was fine while there was one
	// staircase and is unusable now that there are two at different resolutions - index 3 of the
	// collision profile and index 3 of the skin are at completely different heights. Expressed
	// against Z it is the same line through the same two points: at the lowest step it is 0 (the
	// bank runs from the halfway line) and it reaches 1 at Z = Height. The v9 note about NOT
	// re-spacing when the cove drops its top slivers is preserved automatically - the truncation
	// changes which heights exist, not where a given height sits along the field.
	const float SafeHeight = FMath::Max(1.f, Height);
	auto BankAlpha = [SafeHeight](float Z)
	{
		return FMath::Clamp((Z - TraceArenaConstants::CoveVisualRiser) / SafeHeight, 0.f, 1.f);
	};

	// Emissive 0.012, the DAIS number, not the 0.026-0.030 the cover blocks carry - and for exactly
	// the reason recorded on the dais: a terrace is a large UP-FACING surface, up-facing surfaces
	// also catch the most key light, and the two terms stack. The palette comment one screen up
	// records what that costs (platform tops blowing out to a flat pale sheet at ~200/255, judged
	// from a screenshot of a platform top rather than of a wall), and the banks are by area the
	// largest up-facing structure in the arena - four of them, 1500 uu deep, the full half length.
	UMaterialInstanceDynamic* BodyMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.f,
			TraceArenaConstants::NeonNeutral, 0.012f)
		: nullptr;

	// Which skin shells carry a glowing contour. v9 put one on every terrace, which was the whole
	// shape read when there were nine of them; at CoveVisualRiser there are 55 and one per shell is
	// the terraced look the user is asking to be rid of, drawn in neon. See CoveContourSpacing.
	//
	// SPACING 0 IN THE BEFORE ARM, i.e. one contour per terrace, which is what v9 drew. The arm has
	// to stay the thing it was: thinning its contours would make the red arm look better than the
	// build the user complained about, and an A/B whose red arm has been quietly improved proves
	// nothing.
	TArray<bool> SkinContour;
	TraceMarkCoveContours(Skin,
		bBuildingSquareCorners ? 0.f : TraceArenaConstants::CoveContourSpacing, SkinContour);

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
			// Y: the lowest step still reaches BankDepth in from the sideline; the ones above it step
			// in along the ellipse, so the treads are wide and shallow near the floor and tighten as
			// the bank sweeps up into the wall.
			//
			// X: the lowest step runs from the halfway line to just short of the goal line; higher
			// ones start further out and stop further back, which is what turns a trough along the
			// sideline into a bank that is highest at the CORNER.
			auto BankPiece = [&](float TopZ, float OuterDepth, FVector& OutCentre, FVector& OutSize) -> bool
			{
				const float Alpha = BankAlpha(TopZ);
				const float InnerY = HalfY - OuterDepth;
				const float NearX = Inboard * Alpha;
				const float FarX = GoalX - TraceArenaConstants::BankGoalClearance - Setback * Alpha;

				const float SpanX = FarX - NearX;
				const float SpanY = HalfY - InnerY;
				if (SpanX <= 1.f || SpanY <= 1.f)
				{
					return false;
				}

				OutCentre = FVector(XSign * (NearX + SpanX * 0.5f), YSign * (InnerY + SpanY * 0.5f), 0.f);
				OutSize = FVector(SpanX, SpanY, 0.f);
				return true;
			};

			// --- The colliders. Invisible, nested, rising out of the floor -------------------------
			//
			// AddCollisionBlock rather than AddNeonBlock: the drawn half is built separately below, and
			// a box component is a REGISTERED PRIMITIVE while a drawn shell is only an instance. This
			// is the only loop in this function whose iteration count the budget cares about.
			//
			// No pawn standoff shell, exactly as before - AddNeonBlock refused one for these too. A
			// terrace is WALKABLE and a shell would leave players standing on thin air past its edge.
			for (const FTraceCoveStep& Terrace : Terraces)
			{
				FVector Centre;
				FVector Size;
				if (!BankPiece(Terrace.TopZ, Terrace.OuterDepth, Centre, Size))
				{
					continue;
				}

				Centre.Z = Terrace.TopZ * 0.5f;
				Size.Z = Terrace.TopZ;
				AddCollisionBlock(Centre, Size, TEXT("Bank"));
			}

			if (!bBuildVisuals)
			{
				continue;   // Dedicated server: collision only, byte-identical to a client's.
			}

			// --- The skin. Thin nested shells, one instance each ------------------------------------
			for (int32 Index = 0; Index < Skin.Num(); ++Index)
			{
				FVector Centre;
				FVector Size;
				if (!BankPiece(Skin[Index].TopZ, Skin[Index].OuterDepth, Centre, Size))
				{
					continue;
				}

				// A shell covers its own riser band and its own tread, and overlaps down into the
				// shell outside it so no crack can open between them. The one at the toe sits on the
				// floor. See CoveShellOverlap for why these are not solid boxes.
				const float PrevTopZ = (Index > 0) ? Skin[Index - 1].TopZ : 0.f;
				const float ShellBottom = FMath::Max(0.f, PrevTopZ - TraceArenaConstants::CoveShellOverlap);
				const float ShellHeight = FMath::Max(1.f, Skin[Index].TopZ - ShellBottom);

				Centre.Z = ShellBottom + ShellHeight * 0.5f;
				Size.Z = ShellHeight;
				AddMeshBlock(CubeMesh, Centre, Size, BodyMID, /*bCastShadow=*/true, TEXT("BankSkin"));

				if (!SkinContour.IsValidIndex(Index) || !SkinContour[Index])
				{
					continue;
				}

				// An INLAID line, not a lip: CoveContourOut of protrusion instead of LipOut's 12, so
				// it lights the contour without putting a ledge on a surface that is trying to read
				// as continuous.
				AddMeshBlock(CubeMesh,
					FVector(Centre.X, Centre.Y,
						Skin[Index].TopZ - TraceArenaConstants::CoveContourHeight * 0.5f),
					FVector(Size.X + TraceArenaConstants::CoveContourOut * 2.f,
						Size.Y + TraceArenaConstants::CoveContourOut * 2.f,
						TraceArenaConstants::CoveContourHeight),
					NeonMID, /*bCastShadow=*/false, TEXT("BankContour"));
			}
		}
	}

	// The crest, the crest tread and the narrowest tread are all spelled out because they are the
	// three numbers spec v9 section 10 is answerable on: how high the curve gets, whether a body can
	// stand at the top of it, and whether any step is tight enough for the mantle probe to skip.
	// Spec v10 section 9 adds the two that ITS claim rests on: the drawn riser, and the worst-case
	// gap between where a foot rests and where the surface is drawn.
	int32 ContourCount = 0;
	for (bool bOn : SkinContour)
	{
		ContourCount += bOn ? 1 : 0;
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("Corner banks (smooth elliptical cove, spec v10 s9): %d collision terraces at %.1f uu riser ")
		TEXT("+ %d drawn shells at %.1f uu riser (%d contour lines) per bank, crest %.0f uu of a %.0f uu ")
		TEXT("bank, crest tread %.0f uu off the wall, narrowest drawn tread %.1f uu, worst foot/skin gap ")
		TEXT("%.1f uu, %.0f uu deep, X %.0f..%.0f."),
		Terraces.Num(), (Terraces.Num() > 0) ? Terraces[0].TopZ : 0.f,
		Skin.Num(), (Skin.Num() > 0) ? Skin[0].TopZ : 0.f, ContourCount,
		Terraces.Last().TopZ, Height, Terraces.Last().OuterDepth,
		(Skin.Num() > 1) ? (Skin[Skin.Num() - 2].OuterDepth - Skin.Last().OuterDepth) : Skin.Last().OuterDepth,
		TraceArenaConstants::CoveCollisionRiser - TraceArenaConstants::CoveVisualRiser,
		Depth, 0.f, GoalX - TraceArenaConstants::BankGoalClearance);
}

float ATraceArenaBuilder::WallFilletToeDepth() const
{
	if (!bBuildWallFillets || bBuildingSquareCorners)
	{
		return 0.f;
	}

	// Never allowed to swallow more than a quarter of the half width from each side: the cove is
	// dressing on the edge of the field, and a live edit that made it 5000 uu deep would turn the
	// playfield into a gutter without anything else in this file noticing.
	return FMath::Clamp(WallFilletDepth, 0.f, HalfWidth() * 0.25f);
}

void ATraceArenaBuilder::BuildWallFillets(bool bBuildVisuals)
{
	// ---------------------------------------------------------------------------------------------
	// SPEC v9 SECTION 10 - the cove along the base of every perimeter wall.
	//
	// The corner banks now carry the curve along the side walls between the goal lines. This carries
	// it EVERYWHERE ELSE, and the "everywhere else" list is why it exists at all: both end walls, the
	// endzone stretches of side wall (the banks deliberately stop at the goal line), and the midfield
	// taper where the bank has thinned to a single 39 uu terrace. Without it three quarters of the
	// arena's wall/floor joins would still be square.
	//
	// WHY IT CAN SIMPLY BE LAID OVER THE TOP OF EVERYTHING. Each run is nested boxes rising from
	// Z = 0, so the ground height it produces is a monotone staircase of the horizontal distance from
	// the wall with every jump under StepRise. So are the floor (trivially), the corner banks, and
	// the mode-B approach ramp. The surface a pawn actually walks on is the pointwise MAXIMUM of
	// those, and the max of two functions whose jumps are all under R has jumps all under R. So the
	// union is walkable BY CONSTRUCTION, whatever it happens to cross, and no surface in the arena
	// had to be audited for this. The union is also why the cove is invisible under the corner banks
	// (which are taller everywhere they exist) instead of fighting them.
	//
	// THE ONE PLACE THAT IS NOT FREE is the mode-B carry-in ramp, because there the union being
	// walkable is not enough - a step poking up through the ramp would be a lip across the run-up to
	// the hoop. Steps that would do that are dropped from the end walls; see the clamp below.
	// ---------------------------------------------------------------------------------------------

	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();
	const float ToeDepth = WallFilletToeDepth();
	const float Height = FMath::Max(0.f, WallFilletHeight);

	// SPEC v10 SECTION 9 - two resolutions of ONE curve. Steps is what the pawn stands on and is a
	// box component apiece (the registered-primitive budget); Skin is what the eye sees and is an
	// instance apiece (free). Both stop rules are scale-free, so the envelope is identical - see
	// TraceBuildCoveProfile.
	TArray<FTraceCoveStep> Steps;
	TArray<FTraceCoveStep> Skin;
	TraceBuildCoveProfile(ToeDepth, Height, TraceArenaConstants::CoveCollisionRiser,
		TraceArenaConstants::FilletMinInner, TraceArenaConstants::FilletMaxSlope, Steps);
	TraceBuildCoveProfile(ToeDepth, Height, TraceArenaConstants::CoveVisualRiser,
		TraceArenaConstants::FilletMinInner, TraceArenaConstants::FilletMaxSlope, Skin);

	if (Steps.Num() == 0 || Skin.Num() == 0)
	{
		return;
	}

	// --- The END walls take the FULL cove again (spec v28 §8) --------------------------------------
	//
	// There used to be a clamp here, and deleting it is part of this pass rather than a tidy-up. The
	// mode-B carry-in ramp used to climb the last 940 uu of floor IN FRONT OF EACH END WALL, and this
	// cove occupies the same 600 uu, so every end-wall step that would have poked up through the
	// ramp's surface was dropped - a lip across the only way into the goal on foot is not something
	// to discover mid-run. On the shipped numbers that truncated the end cove at ~69 uu against the
	// side walls' ~190.
	//
	// The ramps are at the GOAL LINE now, 2400 uu clear of the wall, and nothing else stands in the
	// pocket's last 600 uu. Keeping the clamp would have cost the two end walls two thirds of their
	// curve to avoid a collision that can no longer happen - and, worse, it would have been a rule
	// nobody could see the reason for, which is how a stale special case survives a decade.
	const int32 EndSteps = Steps.Num();
	const int32 EndSkin = Skin.Num();

	// --- Materials --------------------------------------------------------------------------------
	//
	// NEUTRAL CYAN, not the per-half team colour the banks and the flank dressing wear, and that is a
	// decision rather than a shortcut. This is the single line that runs unbroken round the entire
	// perimeter of the arena - it is the arena's OUTLINE - and an outline that changes colour at the
	// halfway line stops reading as one shape. It also keeps this geometry completely out of the
	// half-time repaint: nothing built here is registered with RegisterSideMID, so a side switch
	// cannot touch it, and there is no way for the cove to end up lit for the wrong end.
	//
	// The body carries the same faint self-lit tint the walls do (0.022) rather than the terrace
	// number (0.012): a cove is a large up-facing surface like a terrace, but unlike a terrace it is
	// something a first-person eye ends up pressed into while sliding along a wall, and emissive is
	// the only term that does not depend on an angle of incidence at that range.
	UMaterialInstanceDynamic* BodyMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::WallColor, 0.45f, 0.f, TraceArenaConstants::NeonNeutral, 0.022f)
		: nullptr;
	UMaterialInstanceDynamic* NeonMID = bBuildVisuals
		? MakeNeonMID(TraceArenaConstants::NeonNeutral, TraceArenaConstants::GlowLip)
		: nullptr;

	// Every run buries its inner edge WallThickness INTO the wall it hugs, so there is no seam at the
	// join for a capsule to catch on and no z-fighting sliver where the cove meets the wall face.
	const float Bury = WallThickness;

	// Which drawn shells carry a glowing contour. See CoveContourSpacing: one per shell would be 38
	// neon lines up a 296 uu curve, which is a terraced look drawn in light whatever the geometry is.
	TArray<bool> SkinContour;
	TraceMarkCoveContours(Skin, TraceArenaConstants::CoveContourSpacing, SkinContour);

	// --- The colliders ----------------------------------------------------------------------------
	//
	// Side walls first, full length, running THROUGH both end walls, so the four corners are closed
	// by these alone and the end runs can simply cross them. Overlapping solids are free here - the
	// union is what matters and it is provably walkable.
	//
	// AddCollisionBlock rather than AddNeonBlock, because spec v10 section 9 draws this cove
	// separately and at eight times the resolution; a box component is a registered primitive and a
	// drawn shell is not, so these are the only iterations the budget counts.
	for (const float YSign : { -1.f, 1.f })
	{
		for (const FTraceCoveStep& Step : Steps)
		{
			const float SpanY = Step.OuterDepth + Bury;

			AddCollisionBlock(
				FVector(0.f, YSign * (HalfY + Bury - SpanY * 0.5f), Step.TopZ * 0.5f),
				FVector(FieldLength + 2.f * WallThickness, SpanY, Step.TopZ),
				TEXT("WallFilletY"));
		}
	}

	for (const float XSign : { -1.f, 1.f })
	{
		for (int32 Index = 0; Index < EndSteps; ++Index)
		{
			const FTraceCoveStep& Step = Steps[Index];
			const float SpanX = Step.OuterDepth + Bury;

			AddCollisionBlock(
				FVector(XSign * (HalfX + Bury - SpanX * 0.5f), 0.f, Step.TopZ * 0.5f),
				FVector(SpanX, FieldWidth, Step.TopZ),
				TEXT("WallFilletX"));
		}
	}

	if (!bBuildVisuals)
	{
		return;   // Dedicated server: collision only, byte-identical to a client's.
	}

	// --- The skin ---------------------------------------------------------------------------------
	//
	// Thin nested shells rather than solid boxes: at CoveVisualRiser a side-wall run is 38 of them,
	// and 38 nested 33600 x 600 uu slabs would be a wall of overdraw and 38 full-height shadow
	// casters for a shape 296 uu tall. See CoveShellOverlap.
	auto ShellBottomZ = [](const TArray<FTraceCoveStep>& Profile, int32 Index)
	{
		const float PrevTopZ = (Index > 0) ? Profile[Index - 1].TopZ : 0.f;
		return FMath::Max(0.f, PrevTopZ - TraceArenaConstants::CoveShellOverlap);
	};

	for (const float YSign : { -1.f, 1.f })
	{
		for (int32 Index = 0; Index < Skin.Num(); ++Index)
		{
			const FTraceCoveStep& Step = Skin[Index];
			const float SpanY = Step.OuterDepth + Bury;
			const float BottomZ = ShellBottomZ(Skin, Index);
			const float ShellHeight = FMath::Max(1.f, Step.TopZ - BottomZ);
			const float CentreY = YSign * (HalfY + Bury - SpanY * 0.5f);

			AddMeshBlock(CubeMesh,
				FVector(0.f, CentreY, BottomZ + ShellHeight * 0.5f),
				FVector(FieldLength + 2.f * WallThickness, SpanY, ShellHeight),
				BodyMID, /*bCastShadow=*/true, TEXT("WallFilletYSkin"));

			if (SkinContour.IsValidIndex(Index) && SkinContour[Index])
			{
				AddMeshBlock(CubeMesh,
					FVector(0.f, CentreY, Step.TopZ - TraceArenaConstants::CoveContourHeight * 0.5f),
					FVector(FieldLength + 2.f * WallThickness,
						SpanY + TraceArenaConstants::CoveContourOut * 2.f,
						TraceArenaConstants::CoveContourHeight),
					NeonMID, /*bCastShadow=*/false, TEXT("WallFilletYContour"));
			}
		}
	}

	for (const float XSign : { -1.f, 1.f })
	{
		for (int32 Index = 0; Index < EndSkin; ++Index)
		{
			const FTraceCoveStep& Step = Skin[Index];
			const float SpanX = Step.OuterDepth + Bury;
			const float BottomZ = ShellBottomZ(Skin, Index);
			const float ShellHeight = FMath::Max(1.f, Step.TopZ - BottomZ);
			const float CentreX = XSign * (HalfX + Bury - SpanX * 0.5f);

			AddMeshBlock(CubeMesh,
				FVector(CentreX, 0.f, BottomZ + ShellHeight * 0.5f),
				FVector(SpanX, FieldWidth, ShellHeight),
				BodyMID, /*bCastShadow=*/true, TEXT("WallFilletXSkin"));

			if (SkinContour.IsValidIndex(Index) && SkinContour[Index])
			{
				AddMeshBlock(CubeMesh,
					FVector(CentreX, 0.f, Step.TopZ - TraceArenaConstants::CoveContourHeight * 0.5f),
					FVector(SpanX + TraceArenaConstants::CoveContourOut * 2.f, FieldWidth,
						TraceArenaConstants::CoveContourHeight),
					NeonMID, /*bCastShadow=*/false, TEXT("WallFilletXContour"));
			}
		}
	}

	int32 ContourCount = 0;
	for (bool bOn : SkinContour)
	{
		ContourCount += bOn ? 1 : 0;
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("Wall fillets (smooth, spec v10 s9): collision %d steps to %.0f uu on the side walls, %d on ")
		TEXT("the end walls (mode-B ramp ceiling), %.1f uu riser. Drawn %d shells to %.0f uu, %d on the ")
		TEXT("end walls, %.1f uu riser, %d contour lines. %.0f uu toe, innermost drawn edge %.0f uu off ")
		TEXT("the wall face, narrowest drawn tread %.1f uu, worst foot/skin gap %.1f uu."),
		Steps.Num(), Steps.Last().TopZ, EndSteps, Steps[0].TopZ,
		Skin.Num(), Skin.Last().TopZ, EndSkin, Skin[0].TopZ, ContourCount,
		ToeDepth, Skin.Last().OuterDepth,
		(Skin.Num() > 1) ? (Skin[Skin.Num() - 2].OuterDepth - Skin.Last().OuterDepth) : Skin.Last().OuterDepth,
		TraceArenaConstants::CoveCollisionRiser - TraceArenaConstants::CoveVisualRiser);
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

		// THE GATE STANDS JUST IN FRONT OF THE GOAL LINE, NOT ON IT (spec v28 §8), and this is the one
		// place the hockey ends cost the old layout something. The gate is two 2300 uu towers on the
		// sidelines carrying a 300 uu beam whose top is flush with the wall tops; the mode-B hoop now
		// hangs ON the goal line and its outer edge reaches Z 2468, i.e. straight through that beam.
		// A hoop with a bar buried across the top of it is not "floating", and the beam is the arena's
		// biggest piece of "you are attacking THAT way" signage, so neither could give way.
		//
		// Moving the gate one ring-outer-radius up the pitch resolves it and reads better than the
		// coincidence did: you run under the gate, then at the goal, then round behind it. Measured
		// FROM THE RING rather than as a fraction of anything, so a bigger hoop pushes the gate off
		// itself instead of growing into it.
		const float GateX = GoalX - Sign * (GoalRingOuterRadius() + TraceArenaConstants::GateGoalClearance);

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
			const FTraceBuildMark EndzonePaintMark = MarkBuiltComponents();

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
			// rather than as more floor.
			//
			// PULLED IN TO THE TOE OF THE WALL COVE (spec v9 section 10). They used to sit ON the
			// sidelines, 22 uu off the wall - which after the cove landed is 22 uu INSIDE it, i.e.
			// buried in solid geometry and contributing nothing at all. They now run along the line
			// where the floor stops being flat and starts curving up into the wall, which is where
			// the eye reads the edge of the room anyway. The rectangle they draw with the goal line
			// is that much narrower than the trigger; the full-width floor patch behind them (which
			// does still reach the sidelines, disappearing under the cove as it goes) is what carries
			// the "the whole width scores" claim.
			const float EdgeInset = WallFilletToeDepth() + TraceArenaConstants::GoalLineWidth * 0.5f;
			for (const float YSign : { -1.f, 1.f })
			{
				AddMeshBlock(CubeMesh,
					FVector(CenterX, YSign * FMath::Max(0.f, HalfY - EdgeInset), TraceArenaConstants::GoalLineZ),
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
				AddPylon(FVector2D(GateX, YSign * TowerY),
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
			AddNeonBlock(FVector(GateX, 0.f, BeamZ),
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
		ZoneParams.ObjectFlags |= SpawnedActorFlags();
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

void ATraceArenaBuilder::BuildGoalRing(float Sign, bool bBuildVisuals)
{
	// ---------------------------------------------------------------------------------------------
	// ONE FREE-STANDING HOOP. Spec v6 §4.3's shape, hung where spec v28 §8 asks for it.
	//
	// GoalRingSegments spokes form an annulus whose inner faces circumscribe the scoring disc; one
	// bright neon band wrapped round the whole thickness of that annulus is the hoop you aim at, and
	// it stands proud on BOTH faces so the goal reads identically from in front of it and from behind
	// it; and a run-up ramp on each face is the way in on foot from either direction.
	//
	// WHAT WENT AWAY WITH THE WALL. Until spec v28 §8 this function was BuildGoalWall and it built a
	// perforated REPLACEMENT end wall: four panels around a square opening, an alcove behind the hole
	// so the hoop framed a lit room rather than the void outside the level, and four pawn shells
	// around the aperture. The wall is 2400 uu further back now and has no hole in it, so all of that
	// is deleted rather than disabled - the hoop frames the pocket, which is real playable floor with
	// the spawn fan and the end wall's own dressing in it.
	//
	// Everything built here is collected into GoalModePieces by the caller and hidden (and
	// de-collided) while mode A is armed.
	// ---------------------------------------------------------------------------------------------

	const float PlaneX = Sign * GoalLineX();
	const float Radius = GoalRingRadius();
	const float OuterRadius = GoalRingOuterRadius();
	const float CentreZ = GoalRingCentreZ();
	const float HalfThickness = GoalRingHalfThickness();

	// Which team defends THIS end, asked of the one function that answers it, so a half-time side
	// switch (which flips TeamEndSign's answer) cannot leave the hoop tinted for the wrong side.
	const ETraceTeam DefendingTeam = (TeamEndSign(ETraceTeam::Blue) == Sign) ? ETraceTeam::Blue : ETraceTeam::Orange;
	const FLinearColor TeamColor = TraceTeamColor(DefendingTeam);

	// The ring body and its rim take the colour of the team that DEFENDS this end, exactly as the
	// endzone patch does, and are registered for the half-time repaint so the sides swap with
	// everything else.
	UMaterialInstanceDynamic* RingBodyMID = bBuildVisuals
		? MakeSurfaceMID(TraceArenaConstants::StructureColor, 0.50f, 0.f, TeamColor, 0.035f)
		: nullptr;
	UMaterialInstanceDynamic* RingNeonMID = bBuildVisuals
		? MakeNeonMID(TeamColor, TraceArenaConstants::GlowGoalRing)
		: nullptr;

	RegisterSideMID(Sign, RingBodyMID, /*bNeon=*/false, /*Intensity=*/0.035f, /*BaseDim=*/-1.f);
	RegisterSideMID(Sign, RingNeonMID, /*bNeon=*/true, TraceArenaConstants::GlowGoalRing);

	// --- 1. The annulus --------------------------------------------------------------------------
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
		// local +Z to the tangent - so Size is (through the ring, radial, tangential).
		const FRotator SpokeRotation(0.f, 0.f, FMath::RadiansToDegrees(Angle));
		const FVector SpokeCentre(PlaneX, MidRadius * CosA, CentreZ + MidRadius * SinA);
		const FVector SpokeSize(HalfThickness * 2.f, RadialDepth, SpokeWidth);

		AddCollisionBlockRotated(SpokeCentre, SpokeSize, TEXT("GoalRingSpoke"), SpokeRotation);

		if (!bBuildVisuals)
		{
			continue;
		}

		// bCastShadow=false, and the reason CHANGED with spec v28 §8 even though the answer did not.
		// It used to be that a spoke lay in the plane of the wall panels around it, so its shadow was
		// always inside theirs. There is no wall now - but a hoop 2400 uu from the end wall, lit by
		// three high directional lights, would throw a 2000 uu ellipse across the goalmouth floor and
		// across the ramp a carrier has to read, for GoalRingSegments x 2 extra shadow casters. The
		// ring is the brightest object at this end of the field; it does not also need to be the
		// darkest.
		AddMeshBlockRotated(CubeMesh, SpokeCentre, SpokeSize, RingBodyMID, /*bCastShadow=*/false,
			TEXT("GoalRingSpoke"), SpokeRotation);

		// THE HOOP. A thin bright band wrapped round the annulus's inner rim, standing GoalRingRimProud
		// past EACH face so the ring is visible edge-on from the halfway line and from the pocket
		// alike. This is the entire answer to "make it visually obvious what and where the goal is",
		// and since spec v28 §8 it is ONE band through the whole thickness rather than one strip on the
		// field-facing side - a goal you can score through from either side may not be brighter from
		// one of them.
		const float RimRadius = Radius + TraceArenaConstants::GoalRingRimDepth * 0.5f;

		// SIZED AT THE RIM'S OWN RADIUS, not at the spoke's. MEASURED: reusing SpokeWidth here (which
		// is sized for the annulus's OUTER radius, where the sectors are widest) made each rim bar 690
		// uu long on a circle that only needs 410 at that radius - so they overshot each other by 68%
		// and the hoop photographed as a jagged pinwheel of crossing bars rather than as a ring. The
		// 1.06 is the seam overlap and nothing more.
		const float RimWidth = 2.f * RimRadius * FMath::Tan(SectorHalfAngle) * 1.06f;

		const FVector RimCentre(PlaneX, RimRadius * CosA, CentreZ + RimRadius * SinA);
		const FVector RimSize(
			HalfThickness * 2.f + TraceArenaConstants::GoalRingRimProud * 2.f,
			TraceArenaConstants::GoalRingRimDepth,
			RimWidth);

		AddMeshBlockRotated(CubeMesh, RimCentre, RimSize, RingNeonMID, /*bCastShadow=*/false,
			TEXT("GoalRingRim"), SpokeRotation);
	}

	// --- 2. The approach ramps, ONE ON EACH FACE ---------------------------------------------------
	//
	// WITHOUT THESE THERE IS NO CARRY-IN GOAL, and that is worth stating plainly because it is a
	// gameplay consequence of a purely visual-sounding instruction. Raising the mouth 1.5 player
	// heights puts the bottom of the hoop at 264 uu; a pawn's origin sits at 88 uu standing and
	// reaches roughly 208 uu at the top of a jump, so a carrier on the flat can never get their origin
	// inside the disc and "scoring by carrying through it" would be dead on arrival. A shallow slope
	// (13 degrees at the default, well inside the walkable angle) up to one step BELOW the hoop puts a
	// standing carrier's origin 952 uu from the ring centre - inside a 1000 uu mouth.
	//
	// SPEC v28 §8 BUILDS IT TWICE, mirrored through the ring plane, because the pocket behind the goal
	// is playable and "allow goals to be scored through either side" has to mean on foot as well as
	// through the air. This is a loop over the two face signs and not a second special case: the ramp
	// is the same object, and the only thing that differs is which way it falls away from the hoop.
	//
	// It also gives the defence something: a ramp is the only way in on foot, each is 2000 uu wide,
	// and they are the obvious places to hold.
	const float RampTop = GoalRampTopZ();
	const float Run = GoalRampRun();
	if (RampTop > 1.f && Run > 1.f)
	{
		const float SlopeLength = FMath::Sqrt(Run * Run + RampTop * RampTop);
		const float SlopeRadians = FMath::Atan2(RampTop, Run);
		const float SlopeDegrees = FMath::RadiansToDegrees(SlopeRadians);

		UMaterialInstanceDynamic* RampMID = bBuildVisuals
			? MakeSurfaceMID(TraceArenaConstants::Dim(TeamColor, 0.035f), 0.30f, 0.f, TeamColor, 0.10f)
			: nullptr;
		RegisterSideMID(Sign, RampMID, /*bNeon=*/false, /*Intensity=*/0.10f, /*BaseDim=*/0.035f);

		// FaceSign is the direction the ramp FALLS AWAY from the ring, in world X: -Sign runs back down
		// the pitch, +Sign runs back into the pocket.
		for (const float FaceSign : { -1.f, 1.f })
		{
			// Positive pitch tilts the block's local +X upward, so a ramp that rises in the -FaceSign
			// direction wants Pitch = -FaceSign * slope.
			const FRotator RampRotation(-FaceSign * SlopeDegrees, 0.f, 0.f);

			// The high end meets the ring's own face; the foot is Run further out. Centre of the sloped
			// slab, dropped half a thickness along its own normal so the TOP surface is the line from
			// the foot to the hoop rather than the middle of the slab.
			const float HighX = PlaneX + FaceSign * HalfThickness;
			const FVector RampCentre(
				HighX + FaceSign * Run * 0.5f - FaceSign * FMath::Sin(SlopeRadians) * TraceArenaConstants::GoalRampThickness * 0.5f,
				0.f,
				RampTop * 0.5f - FMath::Cos(SlopeRadians) * TraceArenaConstants::GoalRampThickness * 0.5f);

			const FVector RampSize(SlopeLength, Radius * 2.f, TraceArenaConstants::GoalRampThickness);

			AddCollisionBlockRotated(RampCentre, RampSize, TEXT("GoalRamp"), RampRotation);

			if (!bBuildVisuals)
			{
				continue;
			}

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
		TEXT("Goal ring (%s end): FLOATING on the goal line at X %.0f, mouth radius %.0f (%.0f uu across), ")
		TEXT("bottom of the mouth %.0f uu up, ring band %.0f uu (outer edge %.0f uu off the floor), %d spokes, ")
		TEXT("two ramps to %.0f uu over %.0f uu of run, %.0f uu of playable pocket behind it."),
		*TraceTeamName(DefendingTeam).ToString(), PlaneX, Radius, Radius * 2.f,
		CentreZ - Radius, OuterRadius - Radius, CentreZ - OuterRadius, Segments, RampTop, Run,
		ClampedEndzoneDepth());
}

void ATraceArenaBuilder::BuildGoals(bool bBuildVisuals)
{
	// ---------------------------------------------------------------------------------------------
	// MODE B - the goals. Spec v6 section 4.3 made them circular and raised them 1.5 player heights;
	// spec v28 section 8 took the wall out from behind them:
	//
	//   "Structure the ends of the field kind of like a hockey field, where you can play behind the
	//    goals. Keep the goals the same, raised in the air floating, just move the wall further back
	//    and put the spawn area there. Allow goals to be scored through either side of the goal."
	//
	// So the goal is a FREE-STANDING HOOP hanging on the goal line (BuildGoalRing), with a run-up ramp
	// on each of its faces, and the scoring volume is a shallow slab STRADDLING its mouth which scores
	// on the DISC inscribed in it, not on the slab's corners. The mouth itself is untouched: same
	// diameter, same height, same disc.
	//
	// The floor dressing is the approach lane, and since v28 it is drawn on BOTH sides of the goal
	// line - the sill on the line, a tinted patch and two rails running away from it up the pitch AND
	// back into the pocket - because "which lane is the goal in" is now a question a carrier asks from
	// behind the goal as well as in front of it. Everything here is tagged into GoalModePieces and
	// hidden (and de-collided) while mode A is being played.
	// ---------------------------------------------------------------------------------------------

	UWorld* World = GetWorld();
	const float MouthHalfY = GoalHalfWidth();
	const float Depth = ClampedEndzoneDepth();
	const float Radius = GoalRingRadius();
	const float CentreZ = GoalRingCentreZ();
	const float SlabHalfDepth = GoalSlabHalfDepth();

	// Mark BEFORE anything is built, collision included: the ring BLOCKS, and on a dedicated server
	// (which builds collision and no visuals at all) an untagged hoop would be a solid obstacle
	// hanging over the goal line through the whole of mode A.
	const FTraceBuildMark GoalMark = MarkBuiltComponents();

	// Say the finished shape out loud, in uu, at Display. This is the line a playtest of spec v6 and
	// v28 is judged against, and none of it - the height off the floor, the diameter, the fact that it
	// is a circle floating clear of a wall that is now 2400 uu behind it, the fact that the slab
	// reaches equally far both ways - is readable from a screenshot.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Arena] MODE B goal (spec v6 + v28 §8): CIRCULAR, %.0f uu across, FLOATING on the goal ")
		TEXT("line at |X| %.0f with %.0f uu of playable pocket behind it, centre Z %.0f, bottom of the ")
		TEXT("mouth %.0f uu off the floor (= %.2f x the %.0f uu player height), bottom of the ring band ")
		TEXT("%.0f uu off the floor, scoring slab %.0f uu deep EITHER SIDE of the ring plane."),
		Radius * 2.f, GoalLineX(), Depth, CentreZ, CentreZ - Radius,
		(PlayerHeightUU() > 0.f) ? ((CentreZ - Radius) / PlayerHeightUU()) : 0.f, PlayerHeightUU(),
		CentreZ - GoalRingOuterRadius(), SlabHalfDepth);

	const ETraceTeam Teams[] = { ETraceTeam::Blue, ETraceTeam::Orange };
	for (const ETraceTeam Team : Teams)
	{
		const float Sign = TeamEndSign(Team);
		const float GoalX = Sign * GoalLineX();
		const FLinearColor TeamColor = TraceTeamColor(Team);

		// The hoop and its two ramps.
		BuildGoalRing(Sign, bBuildVisuals);

		if (!bBuildVisuals)
		{
			continue;
		}

		UMaterialInstanceDynamic* FrameMID = MakeNeonMID(TeamColor, TraceArenaConstants::GlowGoalFrame);
		RegisterSideMID(Sign, FrameMID, /*bNeon=*/true, TraceArenaConstants::GlowGoalFrame);

		UMaterialInstanceDynamic* MouthMID = MakeSurfaceMID(
			TraceArenaConstants::Dim(TeamColor, 0.030f), 0.20f, 0.f, TeamColor, 0.16f);
		RegisterSideMID(Sign, MouthMID, /*bNeon=*/false, /*Intensity=*/0.16f, /*BaseDim=*/0.030f);

		// --- The approach lane on the floor, ON BOTH SIDES OF THE GOAL LINE -----------------------
		//
		// A mouth 264 uu up in the air is not visible on the FLOOR, and the floor is where a carrier's
		// eye spends the run-in. The sill ON the line and the lane running away from it draw the
		// 2000 uu corridor that ends at a ramp, so "the goal is down this lane" is legible from ground
		// level - and since spec v28 §8 that has to be true from the pocket too, so the lane is one
		// loop over the two lane signs rather than one strip pointing up the pitch.
		//
		// The lane is one pocket deep on each side, which is what makes the pair symmetric about the
		// goal line whatever the pocket is set to.
		AddMeshBlock(CubeMesh,
			FVector(GoalX, 0.f, TraceArenaConstants::GoalLineZ + 4.f),
			FVector(TraceArenaConstants::GoalSillWidth, MouthHalfY * 2.f, TraceArenaConstants::GoalLineThickness),
			FrameMID, /*bCastShadow=*/false, TEXT("GoalSill"));

		for (const float LaneSign : { -1.f, 1.f })
		{
			const float LaneCentreX = GoalX + Sign * LaneSign * Depth * 0.5f;

			AddMeshBlock(CubeMesh,
				FVector(LaneCentreX, 0.f, TraceArenaConstants::GoalPatchZ),
				FVector(Depth, MouthHalfY * 2.f, TraceArenaConstants::PatchThickness),
				MouthMID, /*bCastShadow=*/false, TEXT("GoalMouthPatch"));

			for (const float YSign : { -1.f, 1.f })
			{
				AddMeshBlock(CubeMesh,
					FVector(LaneCentreX, YSign * MouthHalfY, TraceArenaConstants::GoalLineZ + 4.f),
					FVector(Depth, TraceArenaConstants::GoalSillWidth * 0.6f, TraceArenaConstants::GoalLineThickness),
					FrameMID, /*bCastShadow=*/false, TEXT("GoalSideRail"));
			}
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

		// CENTRED ON THE HOOP - and since spec v28 §8 that means centred ON THE RING PLANE rather than
		// tucked into the wall in front of it. The volume is the ring's bounding slab and ConfigureRing
		// turns it into a disc; putting the slab's own centre on the mouth is the entire mechanism
		// behind "allow goals to be scored through either side of the goal", because every test that
		// scores works in this box's local space and is symmetric in X once the box is.
		const FTransform ZoneTransform(
			GetActorRotation(),
			GetActorTransform().TransformPosition(FVector(Sign * GoalLineX(), 0.f, CentreZ)));

		FActorSpawnParameters ZoneParams;
		ZoneParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ZoneParams.ObjectFlags |= SpawnedActorFlags();
		ZoneParams.bDeferConstruction = true;

		ATraceEndzone* Goal = World->SpawnActor<ATraceEndzone>(
			ATraceEndzone::StaticClass(), ZoneTransform, ZoneParams);

		if (Goal == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("ATraceArenaBuilder: failed to spawn the %s goal."), *TraceTeamName(Team).ToString());
			continue;
		}

		Goal->ConfigureZone(Team, FVector(SlabHalfDepth, Radius, Radius), /*bInGoalVolume=*/true);
		Goal->ConfigureRing(Radius);
		Goal->SetZoneActive(TraceIsGoalMode(ScoringMode));   // see the matching note in BuildEndzones
		Goal->FinishSpawning(ZoneTransform);
		ScoringVolumes.Add(Goal);

		UE_LOG(LogTraceGame, Log,
			TEXT("Goal (%s defends) is a RING of radius %.0f uu centred at X %.0f, Y 0, Z %.0f, ")
			TEXT("slab X %.0f..%.0f - %.0f uu of it on each side of the ring plane, so it scores from ")
			TEXT("either direction (%.0f uu across = %.0f%% of the %.0f uu field width)."),
			*TraceTeamName(Team).ToString(), Radius,
			Sign * GoalLineX(), CentreZ,
			Sign * GoalLineX() - SlabHalfDepth, Sign * GoalLineX() + SlabHalfDepth, SlabHalfDepth,
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
	const float Spread = HalfWidth() * TraceArenaConstants::StartSpreadFraction;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= SpawnedActorFlags();   // Runtime scaffolding, except during a bake - see SpawnedActorFlags.

	const ETraceTeam Teams[] = { ETraceTeam::Blue, ETraceTeam::Orange };
	for (const ETraceTeam Team : Teams)
	{
		const float Sign = TeamEndSign(Team);

		// SPEC v28 §8: "Set the spawns back behind the goals." A team now spawns in the POCKET behind
		// the goal it defends, and faces up the field - which means facing straight through its own
		// hoop, the single most legible "that way" a spawn can point at.
		//
		// THE LINE IS ASKED FOR, NOT DERIVED HERE. GetSpawnLineX() owns the band between the foot of
		// the back approach ramp and the end wall's pawn standoff, and ATraceGameMode's deeper respawn
		// pads come out of the same function at a different alpha. That is deliberate and it is the
		// lesson ClampedEndzoneDepth already taught this file: the moment two places compute "where is
		// it safe to stand back there" independently, one of them is eventually wrong and the symptom
		// is a pawn spawned inside geometry, which nothing logs.
		//
		// WHAT THE OLD LINE WAS, for the record: HalfX * (1 - StartInsetFraction) - EndzoneDepth, i.e.
		// 1680 uu IN FRONT of the goal line. StartInsetFraction is now unused by this file and the
		// pairing note it carried - pads are goal-relative, so length-relative cover slides through
		// them - is preserved by ApproachCover still being measured back from the goal line.
		//
		// The lateral spread is unchanged: StartSpreadFraction (0.6) of the half width, i.e. +/-2880
		// on the 9600 uu field, which clears the corner banks by 420 uu.
		//
		// THE RESULTING FAN, on the 38400 x 9600 field: X = +/-18314, Y = 0 / +/-1440 / +/-2880, in a
		// pocket that runs from the goal line at 16800 to the end wall at 19200. Re-checked against
		// everything that stands back there: the back ramp's foot is at 17841, so the nearest pad is
		// 473 uu clear of the slope; the corner pylons sit at (18000, +/-4080) with a 300 uu side, so
		// the outermost pad row is 1050 uu clear of them in Y and could sit on top of them in X
		// without touching; the hoop is 1514 uu away in X and 264 uu up. Nothing else is built there.
		// ATraceGameMode's deeper respawn pads come out of the same band at alpha 0.85 (X = 18892).
		const float LineX = GetSpawnLineX(Sign, TraceArenaConstants::StartPocketAlpha);
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

	// SAID OUT LOUD, AT Log, AND WITH THE GOAL PLANE NEXT TO IT. "Are the spawns actually behind the
	// goals?" is spec v28 §8's headline question and it is not answerable from a count - the old line
	// printed the count alone and would have read exactly the same with the pads still in front of the
	// hoop. Both X values on one line is the whole check: |pad| > |goal plane| means behind it.
	UE_LOG(LogTraceGame, Log,
		TEXT("Arena spawned %d player starts per team at |X| %.0f, which is %.0f uu BEHIND the goal ")
		TEXT("plane at |X| %.0f, in a %.0f uu pocket that ends at the wall at |X| %.0f. Lateral fan +/-%.0f."),
		PerTeam, FMath::Abs(GetSpawnLineX(1.f, TraceArenaConstants::StartPocketAlpha)),
		FMath::Abs(GetSpawnLineX(1.f, TraceArenaConstants::StartPocketAlpha)) - GoalLineX(),
		GoalLineX(), ClampedEndzoneDepth(), HalfLength(), Spread);
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
	SpawnParams.ObjectFlags |= SpawnedActorFlags();

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

			// The one shadow caster is the one the Shadows quality row drives - see ApplyFidelity.
			// Remembered here rather than re-found later, because "which light casts the shadows" is
			// decided by the table above and nowhere else.
			if (Spec.bCastShadows)
			{
				KeyLightComponent = LightComponent;
			}
		}

		// The table above is the ONLY place that knows this light is the fill and that one is the
		// bounce - the actors themselves are four identical ADirectionalLights differing in rotation
		// and colour. A bake has to label them for a human reading the World Outliner, so the name
		// travels out as an actor tag, which is a UPROPERTY and therefore survives into the .umap.
		// Bake-only: a tag on every runtime light would be dead weight in a match.
		if (bBakingToLevel)
		{
			Light->Tags.Add(FName(Spec.Name));
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
	SpawnParams.ObjectFlags |= SpawnedActorFlags();

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

	// Everything above is the art direction and is the same at every quality level. Everything that
	// COSTS - ambient occlusion, Lumen, the bloom method, SSR quality - is written by ApplyFidelity
	// instead, from the scalability levels, and re-written whenever they change. Keeping the two
	// apart is what stops a quality change from quietly editing the look.
	ArenaPostProcess = Volume;

	SpawnedActors.Add(Volume);
}

// =================================================================================================
// SPEC v11 §3 - THE FIDELITY LADDER
//
// One function writes every expensive renderer feature this arena can turn on, from the engine's
// scalability levels, into the arena's own post-process volume and lights. Read the block above
// ApplyFidelity() in the header for the group->feature mapping; what follows is why each rung is
// where it is.
//
// THE RULE THIS FILE OBEYS, from spec v11 §0: nothing that costs frames may be on by default. Every
// ladder below therefore has its cheap end at tier 0 and its expensive end at tier 3, and the two
// genuinely expensive features - Lumen and FFT bloom - do not appear at all below tier 2 and tier 3
// respectively. Turning the quality DOWN in this file always removes work; it never substitutes one
// expensive thing for another.
//
// THE OVERRIDE CVARS EXIST FOR MEASUREMENT, not for shipping. Trace.Arena.Fidelity forces every
// group to one tier; Trace.Arena.Fidelity.<Feature> forces one group and leaves the rest following
// scalability. Both default to -1, "follow the scalability level", which is what a player's video
// settings drive. Trace.Arena.FidelityAB uses them to interleave one-feature-at-a-time arms in a
// single process, which is the only way to get a per-feature cost that is not contaminated by a
// second UE run starting up inside the sample window (see the Trace.Arena.PerfAB header).
// =================================================================================================

namespace
{
	/** -1 = follow scalability. 0..3 = force Low/Medium/High/Epic on every group at once. */
	int32 GArenaFidelity = -1;

	/** Per-group overrides. -1 = follow this group's scalability level (then the master above). */
	int32 GArenaFidelityGI = -1;
	int32 GArenaFidelityReflections = -1;
	int32 GArenaFidelityAO = -1;
	int32 GArenaFidelityShadows = -1;
	int32 GArenaFidelityBloom = -1;
	int32 GArenaFidelityLamps = -1;

	/** Every game/PIE world's arena re-reads the ladder. Cheap: a few dozen property writes. */
	void ApplyArenaFidelityEverywhere()
	{
		if (GEngine == nullptr)
		{
			return;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() != nullptr)
			{
				ATraceArenaBuilder::ApplyFidelityInWorld(Context.World());
			}
		}
	}

	void OnArenaFidelityCVarChanged(IConsoleVariable* /*Changed*/)
	{
		ApplyArenaFidelityEverywhere();
	}

	FAutoConsoleVariableRef CVarArenaFidelity(
		TEXT("Trace.Arena.Fidelity"),
		GArenaFidelity,
		TEXT("-1 (default) = every arena fidelity group follows its own scalability level, i.e. the video ")
		TEXT("settings. 0/1/2/3 = force Low/Medium/High/Epic on ALL of them. For measurement (spec v11 3)."),
		FConsoleVariableDelegate::CreateStatic(&OnArenaFidelityCVarChanged), ECVF_Default);

	FAutoConsoleVariableRef CVarArenaFidelityGI(
		TEXT("Trace.Arena.Fidelity.GI"),
		GArenaFidelityGI,
		TEXT("-1 = follow the Global Illumination scalability level. 0/1 = no dynamic GI (the shipped ")
		TEXT("look). 2 = Lumen GI at half quality. 3 = Lumen GI at full quality. THE EXPENSIVE ONE."),
		FConsoleVariableDelegate::CreateStatic(&OnArenaFidelityCVarChanged), ECVF_Default);

	FAutoConsoleVariableRef CVarArenaFidelityReflections(
		TEXT("Trace.Arena.Fidelity.Reflections"),
		GArenaFidelityReflections,
		TEXT("-1 = follow the Reflections scalability level. 0..2 = screen-space reflections at rising ")
		TEXT("quality (2 is the shipped look). 3 = Lumen reflections, but only if GI is already at 2+."),
		FConsoleVariableDelegate::CreateStatic(&OnArenaFidelityCVarChanged), ECVF_Default);

	FAutoConsoleVariableRef CVarArenaFidelityAO(
		TEXT("Trace.Arena.Fidelity.AO"),
		GArenaFidelityAO,
		TEXT("-1 = follow the Post Processing scalability level. 0 = no ambient occlusion (the shipped ")
		TEXT("state - r.DefaultFeature.AmbientOcclusion is False). 1..3 = SSAO at rising quality."),
		FConsoleVariableDelegate::CreateStatic(&OnArenaFidelityCVarChanged), ECVF_Default);

	FAutoConsoleVariableRef CVarArenaFidelityShadows(
		TEXT("Trace.Arena.Fidelity.Shadows"),
		GArenaFidelityShadows,
		TEXT("-1 = follow the Shadows scalability level. 0 = the key light casts NO shadows at all (no ")
		TEXT("cascade pass). 1/2/3 = 2/3/4 cascades at rising resolution; 3 adds contact shadows."),
		FConsoleVariableDelegate::CreateStatic(&OnArenaFidelityCVarChanged), ECVF_Default);

	FAutoConsoleVariableRef CVarArenaFidelityBloom(
		TEXT("Trace.Arena.Fidelity.Bloom"),
		GArenaFidelityBloom,
		TEXT("-1 = follow the Post Processing scalability level. 0..2 = standard sum-of-Gaussians bloom ")
		TEXT("(the shipped look). 3 = FFT convolution bloom. Bloom itself is never switched off: it is ")
		TEXT("what makes an emissive strip read as neon."),
		FConsoleVariableDelegate::CreateStatic(&OnArenaFidelityCVarChanged), ECVF_Default);

	FAutoConsoleVariableRef CVarArenaFidelityLamps(
		TEXT("Trace.Arena.Fidelity.Lamps"),
		GArenaFidelityLamps,
		TEXT("-1 = follow the Effects scalability level. 0..3 scales the floor-lamp lattice's attenuation ")
		TEXT("radius from 45%% to 100%% of what it was built at. Each lamp is an unshadowed deferred light, ")
		TEXT("so its cost is its SCREEN FOOTPRINT and the radius is the dial on that."),
		FConsoleVariableDelegate::CreateStatic(&OnArenaFidelityCVarChanged), ECVF_Default);

	/** Every arena fidelity group's resolved tier, 0..3. */
	struct FArenaFidelityTiers
	{
		int32 GI = 0;
		int32 Reflections = 0;
		int32 AO = 0;
		int32 Shadows = 0;
		int32 Bloom = 0;
		int32 Lamps = 0;
	};

	/** Per-feature override, then the master override, then the scalability level. Always 0..3. */
	int32 ResolveArenaTier(int32 GroupLevel, int32 FeatureOverride)
	{
		if (FeatureOverride >= 0)
		{
			return FMath::Clamp(FeatureOverride, 0, 3);
		}
		if (GArenaFidelity >= 0)
		{
			return FMath::Clamp(GArenaFidelity, 0, 3);
		}

		// Clamped rather than indexed raw: scalability groups can legally read 4 ("Cine") and every
		// ladder in this file is four rungs long. Cine gets Epic, which is the honest answer - this
		// file has nothing above Epic to give it.
		return FMath::Clamp(GroupLevel, 0, 3);
	}

	FArenaFidelityTiers ResolveArenaFidelityTiers()
	{
		const Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();

		FArenaFidelityTiers Tiers;
		Tiers.GI = ResolveArenaTier(Levels.GlobalIlluminationQuality, GArenaFidelityGI);
		Tiers.Reflections = ResolveArenaTier(Levels.ReflectionQuality, GArenaFidelityReflections);
		Tiers.AO = ResolveArenaTier(Levels.PostProcessQuality, GArenaFidelityAO);
		Tiers.Shadows = ResolveArenaTier(Levels.ShadowQuality, GArenaFidelityShadows);
		Tiers.Bloom = ResolveArenaTier(Levels.PostProcessQuality, GArenaFidelityBloom);
		Tiers.Lamps = ResolveArenaTier(Levels.EffectsQuality, GArenaFidelityLamps);
		return Tiers;
	}

	const TCHAR* ArenaTierName(int32 Tier)
	{
		static const TCHAR* const Names[] = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic") };
		return Names[FMath::Clamp(Tier, 0, 3)];
	}

	/**
	 * r.GenerateMeshDistanceFields, or -1 if the cvar is not registered.
	 *
	 * LUMEN'S SOFTWARE RAY TRACING HAS NOTHING TO TRACE AGAINST WITHOUT IT. The project currently
	 * ships it False (Config/DefaultEngine.ini), and this arena is not Nanite and never will be (see
	 * the note in ApplyFidelity), so with distance fields off Lumen builds a surface cache it cannot
	 * reach and the GI it produces is the sky light and nothing else - at full price. That is exactly
	 * the "costs 4 ms and you cannot see it" failure spec v11 §4 asks to be reported rather than
	 * shipped, so the state is logged whenever Lumen is armed instead of being left to a screenshot.
	 */
	int32 ArenaMeshDistanceFieldsEnabled()
	{
		if (const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GenerateMeshDistanceFields")))
		{
			return CVar->GetInt();
		}
		return -1;
	}

	/**
	 * THE RUNG AT WHICH LUMEN ARMS, AND WHY IT IS EPIC AND NOT HIGH.
	 *
	 * Spec v11 §0: "Nothing that costs frames may become the default without an auto-detect deciding
	 * it." The shipped default in Config/DefaultGameUserSettings.ini is HIGH on every group (tier 2),
	 * and on this Mac it stays High forever because the Metal synthetic benchmark is broken and the
	 * settings class falls back to the shipped default rather than trusting its garbage reading. So
	 * "Lumen at tier 2" and "Lumen by default" are the same sentence here, and the spec forbids it.
	 *
	 * At tier 3 the ladder reads: LOW/MEDIUM/HIGH = no GI (High reproduces the shipped look exactly,
	 * which is also what makes "did the fidelity pass regress the default frame?" answerable with a
	 * yes/no), EPIC = Lumen. A player opts in with one keypress on the VIDEO page, and an auto-detect
	 * that picks Epic on a strong GPU is the one path allowed to opt in for them.
	 *
	 * The honest cost of that choice: GLOBAL ILLUMINATION now has three rungs that render the same
	 * thing. That is not a knob that does nothing — it is a binary feature drawn on a four-rung
	 * ladder, because Lumen is the only GI implementation this project has, and a half-priced Lumen
	 * still pays the whole scene-representation cost, which is most of it on a scene this simple.
	 */
	constexpr int32 ArenaLumenMinGITier = 3;

	/**
	 * True when Lumen GI should actually be armed: the tier asked for it AND the project has mesh
	 * distance fields for its software tracing to hit.
	 *
	 * One function so that ApplyFidelity and DescribeFidelity can never disagree — a log line that
	 * claims Lumen on a frame that does not have it is worse than no log line.
	 */
	bool ArenaLumenGIArmed(int32 GITier)
	{
		return (GITier >= ArenaLumenMinGITier) && (ArenaMeshDistanceFieldsEnabled() != 0);
	}
}

void ATraceArenaBuilder::ApplyFidelityInWorld(const UWorld* World)
{
	if (ATraceArenaBuilder* Builder = Get(World))
	{
		Builder->ApplyFidelity();
	}
}

FString ATraceArenaBuilder::DescribeFidelity() const
{
	const FArenaFidelityTiers Tiers = ResolveArenaFidelityTiers();

	// Shares ArenaLumenGIArmed with ApplyFidelity, so a log line can never claim Lumen on a build
	// where mesh distance fields hold it off. See the gate for why that is the prerequisite.
	const bool bLumenGI = ArenaLumenGIArmed(Tiers.GI);

	// Three distinct states, and the third one is the one worth spelling out: a tier that ASKED for
	// Lumen and did not get it looks identical in a screenshot to a tier that never asked.
	const TCHAR* const GIState = bLumenGI
		? TEXT("Lumen")
		: ((Tiers.GI >= ArenaLumenMinGITier) ? TEXT("Lumen HELD OFF - no mesh distance fields") : TEXT("none"));

	// AO PRINTS ITS ARMED STATE, NOT ITS TIER, and the distinction is the whole point of the AO
	// block: the tier can read Epic while ambient occlusion is off, because it is only armed where
	// Lumen makes it free. "AO=Epic" on a frame with no AO in it is exactly the kind of log line
	// that costs an afternoon.
	return FString::Printf(
		TEXT("GI=%s(%s) Reflections=%s(%s) AO=%s Shadows=%s Bloom=%s(%s) Lamps=%s"),
		ArenaTierName(Tiers.GI), GIState,
		ArenaTierName(Tiers.Reflections),
		(bLumenGI && Tiers.Reflections >= 3) ? TEXT("Lumen")
			: ((Tiers.Reflections <= 0) ? TEXT("off") : TEXT("SSR")),
		(bLumenGI && Tiers.AO >= 3) ? TEXT("on(Lumen short-range)") : TEXT("off"),
		ArenaTierName(Tiers.Shadows),
		ArenaTierName(Tiers.Bloom), (Tiers.Bloom >= 3) ? TEXT("FFT") : TEXT("SOG"),
		ArenaTierName(Tiers.Lamps));
}

void ATraceArenaBuilder::ApplyFidelity()
{
	// Nothing here renders, and a dedicated server has no post-process volume and no lights to tune.
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	const FArenaFidelityTiers Tiers = ResolveArenaFidelityTiers();

	// -----------------------------------------------------------------------------------------
	// NANITE IS DELIBERATELY ABSENT FROM THIS LADDER, at every tier, and this is the note spec v11
	// §3 asks for rather than a silent omission.
	//
	// The arena is a few hundred INSTANCES of /Engine/BasicShapes/Cube (12 triangles) and Cylinder,
	// already pooled into a few dozen UInstancedStaticMeshComponents by mesh + material + shadow
	// (see the INSTANCING block in the header). Nanite's cluster culling, software rasteriser and
	// visibility-buffer material pass are a fixed per-frame overhead that pays for itself on dense
	// meshes by removing per-triangle work there is none of here - the whole arena is well under
	// what a single Nanite cluster page is sized for. Enabling it would add a rasteriser and a
	// material resolve pass to a frame that is already per-pixel bound, in exchange for culling
	// nothing. It is not enabled, and it should not be enabled if this arena ever doubles in size
	// either; the thing that would change that answer is real art, not more boxes.
	// -----------------------------------------------------------------------------------------

	if (APostProcessVolume* Volume = ArenaPostProcess.Get())
	{
		FPostProcessSettings& PP = Volume->Settings;

		// --- 1. LUMEN GI AND REFLECTIONS -----------------------------------------------------
		//
		// The one that suits the art direction exactly: every neon strip in this arena is an
		// emissive surface, and Lumen is the only path in this project that lets an emissive
		// surface light anything but itself. Everything else - the three directional lights, the
		// 32-lamp lattice, the BounceLight that exists purely to fake floor bounce - is a
		// stand-in for the GI that is switched off.
		//
		// OFF BELOW EPIC, ENTIRELY. Not "cheap Lumen": the method is set to None, so no Lumen
		// scene is built, no surface cache is captured and no card capture runs. A half-priced
		// Lumen would still pay the whole scene-representation cost, which is most of it on a
		// scene this simple. ArenaLumenMinGITier carries the argument for why that rung is Epic and
		// not High; the short version is that High is the SHIPPED DEFAULT and spec v11 §0 forbids a
		// default that costs frames.
		//
		// THE SECOND HALF OF THE GATE IS MESH DISTANCE FIELDS, and it is a genuine prerequisite
		// rather than a safety belt: Lumen's software ray tracing traces against mesh distance
		// fields, and this arena is not Nanite and must not be, so there is no
		// surface-cache-from-Nanite path either. With r.GenerateMeshDistanceFields=0 Lumen builds a
		// surface cache it cannot reach and produces the sky light and nothing else - AT FULL PRICE.
		// That is exactly the "costs 4 ms and you cannot see it" failure spec v11 §4 asks to be
		// reported rather than shipped, so it is refused in code and logged, not left to a
		// screenshot. Config/DefaultEngine.ini now sets it True (it is a restart-and-rebuild
		// project setting, not a runtime one), which is what makes EPIC mean Lumen.
		const bool bDistanceFields = (ArenaMeshDistanceFieldsEnabled() != 0);

		const bool bLumenGI = ArenaLumenGIArmed(Tiers.GI);
		const bool bLumenReflections = bLumenGI && (Tiers.Reflections >= 3);

		if (Tiers.GI >= ArenaLumenMinGITier && !bDistanceFields)
		{
			// Once per process, at Log. It is a configuration fact, not a per-frame problem, and this
			// function runs on every scalability change.
			static bool bWarnedAboutDistanceFields = false;
			if (!bWarnedAboutDistanceFields)
			{
				bWarnedAboutDistanceFields = true;
				UE_LOG(LogTraceGame, Log,
					TEXT("ATraceArenaBuilder: Global Illumination is at %s but r.GenerateMeshDistanceFields ")
					TEXT("is 0, so Lumen is held OFF - it would cost full price and light nothing without a ")
					TEXT("distance-field scene to trace. Set r.GenerateMeshDistanceFields=True in ")
					TEXT("Config/DefaultEngine.ini to make High/Epic mean Lumen."),
					ArenaTierName(Tiers.GI));
			}
		}

		PP.bOverride_DynamicGlobalIlluminationMethod = true;
		PP.DynamicGlobalIlluminationMethod = bLumenGI
			? EDynamicGlobalIlluminationMethod::Lumen
			: EDynamicGlobalIlluminationMethod::None;

		// TIER 0 TURNS THE FLOOR REFLECTION OFF ENTIRELY, and that is a considered Low-preset trade
		// rather than an oversight. SSR is a full-screen stochastic trace over a frame whose lower
		// third is a near-mirror; halving its quality halves its noise budget but not its pass. The
		// arena still reads - the neon is emissive and unaffected - it just loses its mirror.
		PP.bOverride_ReflectionMethod = true;
		PP.ReflectionMethod = bLumenReflections
			? EReflectionMethod::Lumen
			: ((Tiers.Reflections <= 0) ? EReflectionMethod::None : EReflectionMethod::ScreenSpace);

		if (bLumenGI)
		{
			// Always true while ArenaLumenMinGITier is 3, and kept as a named condition rather than
			// folded away so that the two-rung Lumen ladder below survives intact if a future map
			// with real art ever earns a cheaper Lumen at High.
			const bool bEpic = (Tiers.GI >= 3);

			PP.bOverride_LumenSceneLightingQuality = true;
			PP.LumenSceneLightingQuality = bEpic ? 1.0f : 0.5f;
			PP.bOverride_LumenSceneDetail = true;
			PP.LumenSceneDetail = bEpic ? 1.0f : 0.5f;
			PP.bOverride_LumenFinalGatherQuality = true;
			PP.LumenFinalGatherQuality = bEpic ? 1.0f : 0.5f;
			PP.bOverride_LumenSurfaceCacheResolution = true;
			PP.LumenSurfaceCacheResolution = bEpic ? 1.0f : 0.5f;

			// The field is 33600 uu end to end. Tracing all of it would put the far endzone in
			// every probe; 14000 covers a bit over a third of the field, which is well past
			// anything a first-person eye can resolve a bounce from in a scene this dark, and it
			// is the single biggest dial on Lumen's cost here.
			PP.bOverride_LumenSceneViewDistance = true;
			PP.LumenSceneViewDistance = bEpic ? 22000.f : 14000.f;
			PP.bOverride_LumenMaxTraceDistance = true;
			PP.LumenMaxTraceDistance = bEpic ? 22000.f : 14000.f;

			// Screen traces bypass the surface cache where the screen already has the answer,
			// which on a scene whose light sources are mostly ON SCREEN (the neon you are looking
			// at) is both cheaper and more accurate than the cache.
			PP.bOverride_LumenFinalGatherScreenTraces = true;
			PP.LumenFinalGatherScreenTraces = 1;

			// Zero leaking. The sky here is deliberate near-black twilight; letting it leak into
			// unoccluded interiors would raise the floor of every dark surface, which is the grey
			// box the palette comment in the header spends a paragraph warning about.
			PP.bOverride_LumenSkylightLeaking = true;
			PP.LumenSkylightLeaking = 0.f;

			// Albedos here are 0.011-0.07, i.e. almost nothing bounces. A boost is the honest way
			// to get a visible second bounce out of a world painted this dark without repainting
			// it; 1.6 is a nudge, not a repaint (4 is the engine's ceiling).
			PP.bOverride_LumenDiffuseColorBoost = true;
			PP.LumenDiffuseColorBoost = bEpic ? 1.6f : 1.3f;

			PP.bOverride_LumenReflectionQuality = true;
			PP.LumenReflectionQuality = bLumenReflections ? 1.0f : 0.5f;

			// No "distance fields are off" warning here any more: ArenaLumenGIArmed already refused
			// to arm Lumen in that case, so this branch cannot be reached without them. The refusal
			// is logged once at the gate instead.
		}

		// --- 2. AMBIENT OCCLUSION ------------------------------------------------------------
		//
		// Currently off project-wide (r.DefaultFeature.AmbientOcclusion=False), which sets the
		// DEFAULT intensity to zero - a volume override still wins, which is why this works from
		// here without touching DefaultEngine.ini.
		//
		// AO IS ARMED ONLY WHERE IT IS FREE, AND THAT IS THE MOST USEFUL MEASUREMENT IN THIS PASS.
		//
		// MEASURED at r.ScreenPercentage 300, 5 interleaved cycles, against a null arm whose noise
		// floor was 2.4%: Epic SSAO cost +9.0%, and FID_shipped.png and FID_ao.png are the same
		// picture. Not a tuning miss - structural. AO attenuates INDIRECT/AMBIENT light, and this
		// arena is (a) unlit emissive neon, which AO does not touch at all, and (b) near-black
		// albedo (0.011-0.07) under a deliberate near-zero ambient. Darkening almost nothing by 40%
		// is invisible. Spec v11 §3: "anything that cannot be justified by a screenshot should not
		// ship enabled".
		//
		// So the ladder is not "more AO as the tier rises". AO is off at every tier EXCEPT the one
		// where Lumen GI is also armed, and there it is free rather than merely affordable:
		// IndirectLightRendering.cpp:620 skips the SSAO pass entirely when Lumen GI is on
		// (bLumenWantsSSAO / ShouldRenderAOWithLumenGI), substituting Lumen's own short-range AO,
		// which reads the same intensity and radius. That is also why the 'epic' arm - which
		// includes AO - measured CHEAPER than the '+ao' arm: the +9% is only ever paid with Lumen
		// off. A player who hand-builds Custom with POST PROCESSING on Epic and GLOBAL ILLUMINATION
		// below it therefore gets no AO and no bill for it, which is the right answer both ways.
		//
		// Radius is in WORLD SPACE deliberately. The default screen-space radius makes AO grow
		// with proximity, so a cover block would gain a huge contact shadow as you walked up to
		// it; keyed to uu instead, 160 uu is about a player's height and lands where a block meets
		// the floor at every distance.
		{
			const bool bAmbientOcclusion = bLumenGI && (Tiers.AO >= 3);

			PP.bOverride_AmbientOcclusionIntensity = true;
			PP.AmbientOcclusionIntensity = bAmbientOcclusion ? 0.62f : 0.f;
			PP.bOverride_AmbientOcclusionQuality = true;
			PP.AmbientOcclusionQuality = bAmbientOcclusion ? 100.f : 0.f;
			PP.bOverride_AmbientOcclusionRadius = true;
			PP.AmbientOcclusionRadius = bAmbientOcclusion ? 160.f : 0.f;
			PP.bOverride_AmbientOcclusionRadiusInWS = true;
			PP.AmbientOcclusionRadiusInWS = true;
			PP.bOverride_AmbientOcclusionPower = true;
			PP.AmbientOcclusionPower = 2.2f;
			PP.bOverride_AmbientOcclusionBias = true;
			PP.AmbientOcclusionBias = 3.f;

			// No static-lighting AO term: r.AllowStaticLighting is False, so there is no baked
			// lighting for a static fraction to apply to.
			PP.bOverride_AmbientOcclusionStaticFraction = true;
			PP.AmbientOcclusionStaticFraction = 0.f;

			// Fade AO out well before the far wall. Grounding is a near-field effect and a
			// 33600 uu field would otherwise pay for occlusion nobody can resolve.
			PP.bOverride_AmbientOcclusionFadeDistance = true;
			PP.AmbientOcclusionFadeDistance = 9000.f;
			PP.bOverride_AmbientOcclusionMipBlend = true;
			PP.AmbientOcclusionMipBlend = 0.6f;
		}

		// --- 3. SCREEN-SPACE REFLECTIONS -----------------------------------------------------
		//
		// The floor is the signature surface: near-black, roughness 0.16, and the reflection of
		// the neon in it is most of what is in the lower third of any frame. Tier 2 is what the
		// project shipped (quality 100) - it is kept AT tier 2 rather than moved to 3 so that
		// "High" reproduces the shipped look exactly and Epic is the only rung that changes it.
		//
		// Tier 0 does not appear in this table at all - the method is None there (see the reflection
		// method above), so its quality would be a number nothing reads. Irrelevant when Lumen
		// reflections are armed too; written anyway so that switching back down from Epic restores a
		// complete SSR configuration rather than a partial one.
		{
			static const float SSRQuality[4] = { 25.f, 50.f, 100.f, 100.f };
			static const float SSRMaxRoughness[4] = { 0.30f, 0.42f, 0.55f, 0.60f };

			PP.bOverride_ScreenSpaceReflectionQuality = true;
			PP.ScreenSpaceReflectionQuality = SSRQuality[Tiers.Reflections];
			PP.bOverride_ScreenSpaceReflectionMaxRoughness = true;
			PP.ScreenSpaceReflectionMaxRoughness = SSRMaxRoughness[Tiers.Reflections];
		}

		// --- 4. BLOOM METHOD -----------------------------------------------------------------
		//
		// Bloom is never switched off at any tier - an unlit emissive strip without bloom is a
		// flat coloured rectangle and the whole art direction goes with it. What the tier changes
		// is HOW the glow is computed: sum-of-Gaussians (the shipped look) up to High, and FFT
		// convolution at Epic, which convolves the frame with a real lens kernel instead of
		// stacking six blurs, so a bright strip gets a physically shaped falloff and streaks
		// rather than a stack of Gaussian haloes.
		//
		// THE INTENSITY IS NOT THE SAME NUMBER IN THE TWO PATHS. Standard bloom scales by
		// BloomIntensity * BloomGaussianIntensity; convolution scales by BloomIntensity *
		// BloomConvolutionIntensity against an energy-normalised kernel. The convolution number
		// below is set so the two read at a comparable brightness - if Epic looks like a different
		// game rather than a better-resolved one, this is the dial, not BloomIntensity.
		{
			PP.bOverride_BloomMethod = true;
			PP.BloomMethod = (Tiers.Bloom >= 3) ? BM_FFT : BM_SOG;

			if (Tiers.Bloom >= 3)
			{
				PP.bOverride_BloomConvolutionIntensity = true;
				PP.BloomConvolutionIntensity = 0.11f;
				PP.bOverride_BloomConvolutionSize = true;
				PP.BloomConvolutionSize = 1.6f;
				PP.bOverride_BloomConvolutionScatterDispersion = true;
				PP.BloomConvolutionScatterDispersion = 1.f;

				// Half-resolution FFT. The kernel is smooth and the source is a dark frame with a
				// few very bright lines in it; the full-resolution transform costs roughly twice
				// as much and the difference does not survive the tonemapper here.
				PP.bOverride_BloomConvolutionBufferScale = true;
				PP.BloomConvolutionBufferScale = 0.133f;
			}
		}
	}

	// --- 5. SHADOWS ---------------------------------------------------------------------------
	//
	// One shadow-casting light in the whole arena (see the FLightSpec table in BuildLighting), so
	// this ladder is the entire shadow budget.
	//
	// TIER 0 REMOVES THE PASS, not the resolution. A cascade at a low resolution still costs a
	// depth render of every shadow-casting primitive in range per cascade; switching the caster off
	// removes all of it. What is lost is every character's shadow, which is a real gameplay cue -
	// this is a Low-preset trade and it is stated as one.
	//
	// VIRTUAL SHADOW MAPS ARE NOT USED. r.Shadow.Virtual.Enable is a project setting (currently 0)
	// and it is global rather than per-volume, so it is not this file's to set; the measured reason
	// not to ask for it is in the report - VSMs pay for themselves on scenes with many shadow
	// casters and high depth complexity, and this arena has one directional light over a few
	// hundred boxes, which is the case cascades are already good at.
	if (UDirectionalLightComponent* Key = KeyLightComponent.Get())
	{
		const bool bCastShadows = (Tiers.Shadows >= 1);
		Key->SetCastShadows(bCastShadows);

		if (bCastShadows)
		{
			static const int32 Cascades[4] = { 0, 2, 3, 4 };
			// Tier 2 is the engine's own movable-light default (20000 uu), so "High" reproduces the
			// shadows this project has always had exactly, and only Epic changes them.
			static const float ShadowDistance[4] = { 0.f, 9000.f, 20000.f, 28000.f };
			static const float ResolutionScale[4] = { 1.f, 0.5f, 1.0f, 2.0f };

			Key->SetDynamicShadowCascades(Cascades[Tiers.Shadows]);
			Key->SetDynamicShadowDistanceMovableLight(ShadowDistance[Tiers.Shadows]);

			// A steep distribution spends the cascades near the eye, which is where a first-person
			// camera in a 2600 uu-walled arena can actually resolve a shadow edge.
			Key->SetCascadeDistributionExponent(3.f);
			Key->SetCascadeTransitionFraction(0.1f);

			Key->ShadowResolutionScale = ResolutionScale[Tiers.Shadows];

			// CONTACT SHADOWS AT EPIC ONLY. A short screen-space ray from each pixel toward the
			// light, which catches the small contact darkening a 4-cascade map at 24000 uu cannot
			// resolve - the seam where a cover block meets the floor, and a character's feet.
			// Length is a fraction of the screen, not world units, so it costs the same wherever
			// the camera is.
			Key->ContactShadowLength = (Tiers.Shadows >= 3) ? 0.05f : 0.f;
			Key->ContactShadowLengthInWS = false;
		}

		Key->MarkRenderStateDirty();
	}

	// --- 6. THE FLOOR-LAMP LATTICE ------------------------------------------------------------
	//
	// 32 unshadowed point lights a few hundred uu off the deck, and on a per-pixel-bound frame
	// each one is a deferred lighting pass over its own screen footprint. The footprint is
	// quadratic in the attenuation radius, so scaling the radius is a far bigger lever than
	// scaling the intensity, and unlike deleting lamps it thins the cost EVENLY over the field
	// rather than leaving one sideline lit and the other dark (the lattice is built in
	// XFrac/XSign/YFrac/YSign order, so any stride over the array is a spatial bias).
	//
	// Intensity is raised as the radius falls, because these lights are inverse-square: a tighter
	// lamp at the same candela is a dimmer pool at the same place. The compensation is deliberately
	// PARTIAL - a Low arena is a slightly darker arena, and pretending otherwise would mean
	// blowing out the pool directly under each lamp.
	if (FloorLamps.Num() > 0 && BuiltFloorLampRadius > 0.f)
	{
		static const float LampRadiusScale[4] = { 0.45f, 0.70f, 1.0f, 1.0f };
		static const float LampIntensityScale[4] = { 1.45f, 1.15f, 1.0f, 1.0f };

		const float Radius = BuiltFloorLampRadius * LampRadiusScale[Tiers.Lamps];
		const float Intensity = BuiltFloorLampIntensity * LampIntensityScale[Tiers.Lamps];

		for (const TWeakObjectPtr<UPointLightComponent>& LampPtr : FloorLamps)
		{
			if (UPointLightComponent* Lamp = LampPtr.Get())
			{
				Lamp->SetAttenuationRadius(Radius);
				Lamp->SetIntensity(Intensity);
			}
		}
	}

	UE_LOG(LogTraceGame, Log, TEXT("Arena fidelity applied: %s"), *DescribeFidelity());
}

// -------------------------------------------------------------------------------------------------
// Primitive helpers
// -------------------------------------------------------------------------------------------------

EObjectFlags ATraceArenaBuilder::BuiltObjectFlags() const
{
	// RF_Transient during an editor preview and nothing at runtime. This is the flag that keeps the
	// preview out of the .umap: a transient object is skipped when the level is saved, so a user who
	// builds a preview and then saves the map saves the builder actor and none of its geometry.
	//
	// A BAKE RUNS AS A PREVIEW and therefore lands on RF_Transient here, which is correct and load
	// bearing: the components a bake builds are scaffolding it reads and throws away, and the actors
	// it leaves in the level are separate objects made by EmitBakedActors. See SpawnedActorFlags for
	// the one thing a bake genuinely does have to serialise.
	return bBuildingEditorPreview ? RF_Transient : RF_NoFlags;
}

EObjectFlags ATraceArenaBuilder::SpawnedActorFlags() const
{
	return bBakingToLevel ? RF_NoFlags : RF_Transient;
}

bool ATraceArenaBuilder::RecordForBake(const FTraceBakeRecord& Record)
{
	if (!bBakingToLevel)
	{
		return false;
	}

	BakeRecords.Add(Record);
	return true;
}

void ATraceArenaBuilder::AddMeshBlock(UStaticMesh* Mesh, const FVector& LocalCenter, const FVector& Size,
	UMaterialInstanceDynamic* MID, bool bCastShadow, const TCHAR* DebugName, float YawDegrees)
{
	AddMeshBlockRotated(Mesh, LocalCenter, Size, MID, bCastShadow, DebugName, FRotator(0.f, YawDegrees, 0.f));
}

void ATraceArenaBuilder::AddMeshBlockRotated(UStaticMesh* Mesh, const FVector& LocalCenter,
	const FVector& Size, UMaterialInstanceDynamic* MID, bool bCastShadow, const TCHAR* DebugName,
	const FRotator& Rotation)
{
	// Null-checked per the asset rules: a missing engine shape must degrade to "invisible", never
	// to a crash. Collision lives in separate box components, so the arena still plays.
	if (Mesh == nullptr || Root == nullptr)
	{
		return;
	}

	// One transform, computed once, used by both arms. FTransform's (rotation, translation, scale)
	// constructor composes scale-then-rotate-then-translate, which is exactly what a scene component's
	// relative transform does, so the instanced and legacy arms place a block IDENTICALLY. That is the
	// whole safety property of this change: nothing about where anything is has moved.
	const FTransform LocalTransform(Rotation, LocalCenter, Size / TraceArenaConstants::ShapeUnit);

	// SPEC v15 §1 - THE BAKE WATCHES HERE, above both geometry arms rather than inside
	// AddInstancedBlock, so it records the same block whichever arm the build is running: the baked
	// level must not depend on Trace.Arena.Instancing being on.
	if (bBakingToLevel)
	{
		FTraceBakeRecord Record;
		Record.Kind = FTraceBakeRecord::EKind::MeshBlock;
		Record.PieceName = FName(DebugName);
		Record.Transform = LocalTransform;
		Record.BlockMesh = Mesh;
		Record.BlockMID = MID;
		Record.bCastShadow = bCastShadow;
		RecordForBake(Record);
	}

	if (bBuildingInstancedGeometry)
	{
		AddInstancedBlock(Mesh, MID, bCastShadow, LocalTransform, DebugName);
		return;
	}

	// --- Legacy arm: one component per block. Kept only so Trace.Arena.Perf has a BEFORE. -----------

	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
		this, MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), FName(DebugName)),
		BuiltObjectFlags());
	if (Component == nullptr)
	{
		return;
	}

	// Everything is configured BEFORE RegisterComponent: a scene component may be freely posed
	// while unregistered, and registering once with the final state avoids a redundant render and
	// physics update per piece.
	//
	// Movable, unlike everything else the builder now makes, because this arm exists to reproduce the
	// old behaviour exactly - including the mobility that was half the cost.
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetupAttachment(Root);
	Component->SetStaticMesh(Mesh);
	Component->SetRelativeTransform(LocalTransform);
	Component->SetCollisionProfileName(TEXT("NoCollision"));
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(bCastShadow);

	if (MID != nullptr)
	{
		Component->SetMaterial(0, MID);
	}

	Component->RegisterComponent();
}

void ATraceArenaBuilder::AddInstancedBlock(UStaticMesh* Mesh, UMaterialInstanceDynamic* MID, bool bCastShadow,
	const FTransform& LocalTransform, const TCHAR* DebugName)
{
	// Linear search on purpose. The pool count settles in the low dozens, the build does ~1450
	// lookups, and a TMap keyed on a three-field struct would need a hash and an equality operator to
	// save an amount of time too small to measure once, at build, on a load screen.
	int32 PoolIndex = INDEX_NONE;
	for (int32 Index = 0; Index < InstancePools.Num(); ++Index)
	{
		const FTraceInstancePool& Candidate = InstancePools[Index];
		if (Candidate.Mesh.Get() == Mesh && Candidate.MID.Get() == MID && Candidate.bCastShadow == bCastShadow)
		{
			PoolIndex = Index;
			break;
		}
	}

	if (PoolIndex == INDEX_NONE)
	{
		UInstancedStaticMeshComponent* Pooled = NewObject<UInstancedStaticMeshComponent>(
			this, MakeUniqueObjectName(this, UInstancedStaticMeshComponent::StaticClass(), FName(DebugName)),
			BuiltObjectFlags());
		if (Pooled == nullptr)
		{
			return;
		}

		Pooled->SetupAttachment(Root);
		Pooled->SetStaticMesh(Mesh);

		// NO COLLISION, and it matters more here than it did on the single components. An ISM with a
		// collision profile builds one physics body PER INSTANCE, so a careless profile here would
		// create the ~1450 bodies this change is supposed to be removing. The arena's collision has
		// always lived in separate box components (see AddCollisionBlock) precisely so the visible
		// geometry can be free of it.
		Pooled->SetCollisionProfileName(TEXT("NoCollision"));
		Pooled->SetGenerateOverlapEvents(false);
		Pooled->SetCanEverAffectNavigation(false);
		Pooled->SetCastShadow(bCastShadow);

		if (MID != nullptr)
		{
			Pooled->SetMaterial(0, MID);
		}

		// Deliberately NOT registered here - see FlushInstancePools. Instances are appended while the
		// component is unregistered, which is both cheaper (no render state to recreate per block) and
		// the only way to still be able to choose the mobility once the build knows whether anything
		// in this pool is mode-tagged.

		FTraceInstancePool& NewPool = InstancePools.AddDefaulted_GetRef();
		NewPool.Mesh = Mesh;
		NewPool.MID = MID;
		NewPool.bCastShadow = bCastShadow;
		NewPool.Component = Pooled;
		PoolIndex = InstancePools.Num() - 1;
	}

	UInstancedStaticMeshComponent* Pool = InstancePools[PoolIndex].Component.Get();
	if (Pool == nullptr)
	{
		return;
	}

	const int32 InstanceIndex = Pool->AddInstance(LocalTransform, /*bWorldSpace=*/false);

	FTraceBuiltInstance& Record = BuiltInstances.AddDefaulted_GetRef();
	Record.PoolIndex = PoolIndex;
	Record.InstanceIndex = InstanceIndex;
	Record.Transform = LocalTransform;
}

void ATraceArenaBuilder::FlushInstancePools()
{
	int32 Registered = 0;
	int32 Instances = 0;
	int32 MovablePools = 0;

	for (FTraceInstancePool& Pool : InstancePools)
	{
		UInstancedStaticMeshComponent* Component = Pool.Component.Get();
		if (!IsValid(Component) || Component->IsRegistered())
		{
			continue;
		}

		// STATIC unless the scoring-mode toggle has to rewrite an instance in this pool. See
		// FTraceInstancePool::bNeedsRuntimeInstanceUpdates: moving a Static component in a game world
		// is not allowed, and "moving" includes UpdateInstanceTransform.
		Component->SetMobility(Pool.bNeedsRuntimeInstanceUpdates
			? EComponentMobility::Movable
			: EComponentMobility::Static);

		MovablePools += Pool.bNeedsRuntimeInstanceUpdates ? 1 : 0;
		Instances += Component->GetInstanceCount();

		Component->RegisterComponent();
		++Registered;
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[Arena] Instanced geometry: %d instances across %d pooled meshes (%d of them Movable for the "
		     "A/B scoring toggle, the rest Static). That is %d fewer registered primitives than one "
		     "component per block."),
		Instances, Registered, MovablePools, FMath::Max(0, Instances - Registered));
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

	// STATIC (spec v7 §8). These boxes are the arena's collision and not one of them ever moves, but
	// a Movable primitive is held in the physics scene as a KINEMATIC body in the dynamic broadphase
	// rather than in the static tree - ~500 of them, re-queried by every movement sweep, every bullet
	// and every Core throw. Scoring-mode toggling only ever calls SetCollisionEnabled on these, which
	// is a state change and not a move, so it stays legal on a Static component.
	Component->SetMobility(EComponentMobility::Static);
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

	if (bBakingToLevel)
	{
		FTraceBakeRecord Record;
		Record.Kind = FTraceBakeRecord::EKind::CollisionBox;
		Record.PieceName = FName(DebugName);
		Record.SourceComponent = Component;
		RecordForBake(Record);
	}

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

	// Static for the same reason AddCollisionBlock is: a shell that never moves has no business in the
	// dynamic broadphase.
	Component->SetMobility(EComponentMobility::Static);
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

	if (bBakingToLevel)
	{
		FTraceBakeRecord Record;
		Record.Kind = FTraceBakeRecord::EKind::PawnStandoff;
		Record.PieceName = FName(DebugName);
		Record.SourceComponent = Component;
		RecordForBake(Record);
	}

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

	// Remembered so the Effects quality row can thin the lattice live (spec v11 §3) without a
	// rebuild. Weak: the component's Outer is this actor, so it is already GC reachable.
	FloorLamps.Add(Light);

	if (bBakingToLevel)
	{
		FTraceBakeRecord Record;
		Record.Kind = FTraceBakeRecord::EKind::PointLight;
		Record.PieceName = FName(DebugName);
		Record.SourceComponent = Light;
		RecordForBake(Record);
	}

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

	// What every lamp is built at. ApplyFidelity scales against THESE rather than against whatever a
	// lamp currently has, so repeated quality changes cannot ratchet the lattice brighter or wider.
	BuiltFloorLampIntensity = FloorLampIntensity;
	BuiltFloorLampRadius = FloorLampRadius;

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

	// bCastShadow=false on both (spec v7 §8 shadow audit). These are collars around the shaft, 22 and
	// 15 uu proud of a column that casts, and they sit at the very bottom and the very top of it - the
	// plinth's shadow starts at the floor line where the shaft's already is, and the capital's is
	// thrown from 616+ uu up onto whatever the shaft is already shadowing. Two extra shadow draws per
	// cascade per pylon, for a couple of dozen pixels of extra penumbra nobody can find.
	AddMeshBlock(CubeMesh, FVector(Centre.X, Centre.Y, BaseHeight * 0.5f),
		FVector(BaseSide, BaseSide, BaseHeight), BodyMID, /*bCastShadow=*/false, DebugName);
	AddMeshBlock(CubeMesh, FVector(Centre.X, Centre.Y, Height - CapHeight * 0.5f),
		FVector(CapSide, CapSide, CapHeight), BodyMID, /*bCastShadow=*/false, DebugName);

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

	// SPEC v11 §3.6 - THE FRESNEL RIM, AND WHY ROUGHNESS DECIDES WHO GETS IT.
	//
	// M_TraceSurface gained a quality switch that adds a Fresnel edge term at High and Epic material
	// quality and compiles it out below (see generate_content.py). It draws the silhouette of a
	// near-black block for about six instructions, which is the same job the four pieces of face trim
	// on every cover block do for four extra instances each.
	//
	// IT MUST NOT GO ON THE FLOOR. Fresnel brightens a surface as it turns away from the eye, and a
	// floor is viewed at a grazing angle across most of the screen - a rim there is not an edge, it is
	// a white wash over the lower half of the frame, and it would sit exactly on top of the SSR that
	// is the reason the floor is glossy in the first place. The floor is also the ONLY near-mirror in
	// the arena (roughness 0.16; the art-direction block in the header says so and every other call
	// site passes ~0.5), so roughness is a complete and self-maintaining test for "is this the floor" -
	// no new argument, no forty call sites to audit, and a future glossy surface is excluded for the
	// same reason the floor is rather than by having been forgotten.
	MID->SetVectorParameterValue(TEXT("RimColor"), FLinearColor(0.30f, 0.62f, 0.95f));
	MID->SetScalarParameterValue(TEXT("RimStrength"), (Roughness > 0.3f) ? 0.28f : 0.f);

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

ATraceArenaBuilder::FTraceBuildMark ATraceArenaBuilder::MarkBuiltComponents() const
{
	FTraceBuildMark Mark;
	Mark.Components = (Root != nullptr) ? Root->GetAttachChildren().Num() : 0;
	Mark.Instances = BuiltInstances.Num();
	Mark.BakeRecords = BakeRecords.Num();
	return Mark;
}

void ATraceArenaBuilder::CollectPiecesSince(const FTraceBuildMark& Mark, TArray<FTraceModePiece>& Out)
{
	if (Root == nullptr)
	{
		return;
	}

	// MARK, BUILD, DIFF - and no primitive helper had to learn that modes exist. Every factory in
	// this file either does SetupAttachment(Root) or appends to BuiltInstances, both in construction
	// order, so what appeared since the mark IS what the step built. The alternative was threading a
	// "which mode is this for" argument through AddMeshBlock / AddCollisionBlock / AddPawnStandoff /
	// AddNeonBlock / AddPylon, which is five signatures and a default argument nobody would notice
	// getting wrong.
	//
	// THE INSTANCED HALF IS WHY THIS RUNS IN TWO LOOPS. Pooling interleaves blocks from every build
	// step into a handful of shared components, so "the geometry this step made" is no longer a
	// contiguous run of components - it is a contiguous run of INSTANCES scattered across pools, which
	// is exactly what BuiltInstances records.
	const auto& BuiltChildren = Root->GetAttachChildren();
	for (int32 Index = FMath::Max(0, Mark.Components); Index < BuiltChildren.Num(); ++Index)
	{
		USceneComponent* Built = BuiltChildren[Index];
		if (!IsValid(Built))
		{
			continue;
		}

		// The pooled ISMs are shared by every build step, so one of them appearing in this range would
		// mean hiding hundreds of untagged blocks along with the tagged ones. They cannot appear -
		// registration, and therefore attachment, is deferred to FlushInstancePools long after the
		// last collect - but this is the assumption that would fail silently and invisibly if that
		// ordering were ever changed, so it is asserted rather than assumed.
		if (Built->IsA<UInstancedStaticMeshComponent>())
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[Arena] A pooled instanced mesh (%s) was attached before the mode tags were collected. ")
				TEXT("Tagging it would hide every block in its pool. FlushInstancePools must run after every ")
				TEXT("CollectPiecesSince."),
				*Built->GetName());
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

	for (int32 Index = FMath::Max(0, Mark.Instances); Index < BuiltInstances.Num(); ++Index)
	{
		const FTraceBuiltInstance& Built = BuiltInstances[Index];
		if (!InstancePools.IsValidIndex(Built.PoolIndex))
		{
			continue;
		}

		FTraceInstancePool& Pool = InstancePools[Built.PoolIndex];
		UInstancedStaticMeshComponent* Component = Pool.Component.Get();
		if (!IsValid(Component))
		{
			continue;
		}

		// The pool now has to survive being re-transformed at runtime, so it cannot be Static. This is
		// the only place that decides it, and FlushInstancePools reads it - which is why the flush has
		// to come last.
		Pool.bNeedsRuntimeInstanceUpdates = true;

		FTraceModePiece& Piece = Out.AddDefaulted_GetRef();
		Piece.InstancePool = Component;
		Piece.InstanceIndex = Built.InstanceIndex;
		Piece.InstanceTransform = Built.Transform;
		// Instanced pieces are visual only - the pools are built NoCollision - so there is no
		// collision state to remember or restore. The blocking half of a mode-tagged structure is its
		// separate box component, tagged in the loop above.
	}

	// --- THE BAKE'S THIRD RANGE (spec v15 §1) -----------------------------------------------------
	//
	// Same mark/build/diff idea, one list further along. WHICH mode is being collected is read off
	// the destination array's IDENTITY rather than passed in, and that is deliberate: the alternative
	// was a fourth argument on a function with eight call sites, seven of which would have had to
	// repeat the same constant. There are exactly two tagged sets in this class and both are members,
	// so the test is total - anything else is untagged, which is what Always means.
	if (!bBakingToLevel)
	{
		return;
	}

	const ETraceBakedScoringTag Tag = (&Out == &GoalModePieces)
		? ETraceBakedScoringTag::GoalModeOnly
		: ((&Out == &EndzoneModePieces) ? ETraceBakedScoringTag::EndzoneModeOnly : ETraceBakedScoringTag::Always);

	for (int32 Index = FMath::Max(0, Mark.BakeRecords); Index < BakeRecords.Num(); ++Index)
	{
		BakeRecords[Index].ModeTag = Tag;
	}
}

void ATraceArenaBuilder::SetPiecesPresented(const TArray<FTraceModePiece>& Pieces, bool bPresented)
{
	// Pools touched by an instance rewrite, so the render state is recreated once per pool instead of
	// once per instance. A mode toggle can move a couple of hundred instances.
	TSet<UInstancedStaticMeshComponent*> TouchedPools;

	for (const FTraceModePiece& Piece : Pieces)
	{
		if (UInstancedStaticMeshComponent* Pool = Piece.InstancePool.Get(); IsValid(Pool) && Piece.InstanceIndex != INDEX_NONE)
		{
			// HIDING AN INSTANCE IS COLLAPSING IT. There is no per-instance visibility bit on an ISM,
			// so the piece is scaled to zero where it stands and its built transform is written back
			// when its mode is armed. A zero-scaled instance produces degenerate triangles that cover
			// no pixels; it is the standard idiom, and it is why FTraceModePiece remembers the
			// transform rather than trying to recompute it.
			const FTransform Wanted = bPresented
				? Piece.InstanceTransform
				: FTransform(Piece.InstanceTransform.GetRotation(), Piece.InstanceTransform.GetTranslation(), FVector::ZeroVector);

			Pool->UpdateInstanceTransform(Piece.InstanceIndex, Wanted, /*bWorldSpace=*/false,
				/*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
			TouchedPools.Add(Pool);
			continue;
		}

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

	for (UInstancedStaticMeshComponent* Pool : TouchedPools)
	{
		Pool->MarkRenderStateDirty();
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

	// The same two sets on a baked level, where a piece is a whole actor. Exactly one of the two
	// mechanisms is ever non-empty - see BakedEndzoneModeActors - so this costs a pair of empty loops
	// on the procedural path.
	SetBakedActorsPresented(BakedEndzoneModeActors, !bGoalMode);
	SetBakedActorsPresented(BakedGoalModeActors, bGoalMode);

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
	//
	// BOTH COUNTS, and that is not padding. On a baked level the component counts are legitimately
	// zero and the actor counts carry the whole meaning; reporting only the first pair printed
	// "0 endzone pieces hidden, 0 goal pieces shown" over a level that had just correctly hidden 24
	// pieces and shown 84, which is precisely the sort of log line this project has lost a day to.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Arena] Presenting %s: %d endzone pieces %s, %d goal pieces %s (baked actors: %d / %d), ")
		TEXT("%d of %d volumes armed."),
		*TraceScoringModeLabel(ScoringMode),
		EndzoneModePieces.Num(), bGoalMode ? TEXT("hidden") : TEXT("shown"),
		GoalModePieces.Num(), bGoalMode ? TEXT("shown") : TEXT("hidden"),
		BakedEndzoneModeActors.Num(), BakedGoalModeActors.Num(),
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
// Teardown
//
// DestroyBuiltArena used to live inside the #if WITH_EDITOR block below, because the editor preview
// was the only thing that ever needed to take an arena apart. Spec v7 §8's A/B needs it too: the only
// way to compare the instanced and legacy geometry paths WITHOUT comparing two different processes on
// a machine that has other work on it is to rebuild the arena in place between arms. It is still
// never called by the game itself - at runtime the arena is built exactly once per world and EndPlay
// is the only other teardown path there is.
// -------------------------------------------------------------------------------------------------

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

	// The baked equivalents, and the bake's own scratch list. Nothing here DESTROYS a baked actor -
	// they belong to the level, not to this builder - it only forgets them, exactly as the two lines
	// above forget components that have just died. A re-adopt finds them again.
	BakedEndzoneModeActors.Reset();
	BakedGoalModeActors.Reset();
	BakeRecords.Reset();

	// Same story for the fidelity handles (spec v11 §3): the volume and every light have just been
	// destroyed, so ApplyFidelity must find nothing rather than a list of stale weak pointers whose
	// count would misreport how many lamps the lattice has. The scalability subscription is left
	// alone on purpose - the measurement rebuild goes straight into another build, and re-subscribing
	// per rebuild is how a delegate ends up bound twice.
	ArenaPostProcess.Reset();
	KeyLightComponent.Reset();
	FloorLamps.Reset();
	BuiltFloorLampIntensity = 0.f;
	BuiltFloorLampRadius = 0.f;

	// The instance pools are the same story with a sharper edge: their INDICES are the identity of
	// every mode-tagged piece, so a surviving pool entry would have the next build appending into a
	// component that has just been destroyed and handing out indices that mean nothing. A pool that
	// never got as far as FlushInstancePools is not in the attachment tree either, so destroy it here
	// rather than leaking an unregistered component per aborted preview.
	for (FTraceInstancePool& Pool : InstancePools)
	{
		if (UInstancedStaticMeshComponent* Pooled = Pool.Component.Get(); IsValid(Pooled) && !Pooled->IsRegistered())
		{
			Pooled->DestroyComponent();
			++ComponentsDestroyed;
		}
	}
	InstancePools.Reset();
	BuiltInstances.Reset();

	bArenaBuilt = false;
	bEditorPreviewBuilt = false;

	UE_LOG(LogTraceGame, Verbose, TEXT("ATraceArenaBuilder: torn down %d components and %d actors."),
		ComponentsDestroyed, ActorsDestroyed);
}

#if !UE_BUILD_SHIPPING
void ATraceArenaBuilder::RebuildForMeasurement()
{
	// MEASUREMENT ONLY, and the header says so in stronger terms. This is not a live-tuning facility:
	// it destroys and respawns the endzone triggers and the player starts, and it resets the painted
	// side assignment to the builder's default, so a match that survives it is a match whose endzones
	// may be the wrong colour until the next half-time push. Trace.Arena.PerfAB accepts that because
	// it is measuring frame time, not playing a game.

	// NOT ON A BAKED LEVEL (spec v15 §1). This is the one path that reaches BuildArena() without
	// going through EnsureBuilt(), so the pre-baked check there does not cover it — and on
	// /Game/Maps/Arena_Baked the result would be a full second arena constructed on top of the 517
	// pieces already in the .umap: two floors, two sets of walls, and a measurement of a scene that
	// exists nowhere. DestroyBuiltArena would not even clean it up afterwards, because it only walks
	// what THIS actor built.
	if (IsLevelPreBaked())
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Arena] Trace.Arena.Perf's A/B rebuild does nothing on a baked level: the geometry belongs to ")
			TEXT("the .umap, not to this builder, so there is nothing to rebuild and no BEFORE arm to switch to. ")
			TEXT("Run the A/B on /Game/Maps/Arena, which is still fully procedural."));
		return;
	}

	DestroyBuiltArena();
	BuildArena();
}
#endif

// -------------------------------------------------------------------------------------------------
// Editor preview
//
// See the two-click workflow documented on BuildPreviewInEditor in the header. Everything below is
// #if WITH_EDITOR: none of it is compiled into a packaged game, so a preview cannot exist there and
// the runtime path is byte-identical to what it was.
// -------------------------------------------------------------------------------------------------

#if WITH_EDITOR

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

	// A BAKED LEVEL ALREADY HAS ONE. The preview is transient and Clear Preview takes it away again,
	// so this is a warning rather than a refusal — previewing a layout change before re-baking is a
	// perfectly reasonable thing to want. But the viewport is about to show the arena twice, and
	// somebody who does not know that will read the doubled geometry as a bake bug.
	if (bLevelIsPreBaked)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Arena] This level is already baked: the preview you are about to see is a SECOND, transient ")
			TEXT("arena drawn on top of the 517 saved pieces. Press Clear Preview In Editor to remove it. To ")
			TEXT("make a layout change permanent, edit the properties and re-run Scripts/bake-arena.sh --force."));
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

	// THE ONE FOOTGUN A BAKED LEVEL ADDS, and it is silent without this. On /Game/Maps/Arena these
	// properties ARE the arena: change FieldLength and the next build is longer. On a baked level the
	// geometry is already on disk and does not move, but the properties still answer
	// GetFieldBounds(), GetCoreSpawnLocation(), GetScoringBounds() and ClampedEndzoneDepth() — which
	// the bots steer inside, the Core spawns at, and the endzone triggers are re-sized from at load.
	// Editing one here therefore does not resize the arena; it makes the arena and the rules
	// DISAGREE, with the walls in one place and the scoring volumes in another.
	if (bLevelIsPreBaked && PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		const FName Changed = PropertyChangedEvent.GetPropertyName();
		if (Changed != GET_MEMBER_NAME_CHECKED(ATraceArenaBuilder, bLevelIsPreBaked)
			&& Changed != GET_MEMBER_NAME_CHECKED(ATraceArenaBuilder, BakeMaxPiecesPerName)
			&& Changed != GET_MEMBER_NAME_CHECKED(ATraceArenaBuilder, BakePieceGroupSlack))
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Arena] '%s' changed on a builder in a BAKED level. The geometry is already saved in this ")
				TEXT(".umap and will NOT move — but this property still answers GetFieldBounds / ")
				TEXT("GetCoreSpawnLocation / the endzone volume sizes, so the arena you can see and the arena ")
				TEXT("that scores will now disagree. Either put it back, or re-run Scripts/bake-arena.sh --force ")
				TEXT("to regenerate the level from the new numbers."),
				*Changed.ToString());
		}
	}
}

#endif // WITH_EDITOR

// =================================================================================================
// SPEC v7 §8 - THE MEASUREMENT: Trace.Arena.Perf and Trace.Arena.PerfAB
//
// WHY THIS EXISTS AT ALL, given the project already has Trace.Trail.PerfAB. That harness compares
// three TRAIL renderers; this one compares two ARENA GEOMETRY paths and characterises the scene,
// which is a different question and the one spec v7 §8 actually asks: how many primitives is the
// arena handing the renderer, and what does a frame cost. It samples the same four columns from the
// same globals `stat unit` reads, so the two harnesses' numbers are directly comparable.
//
// TWO COMMANDS, AND THE SECOND ONE IS THE ONE TO TRUST:
//
//   Trace.Arena.Perf    - characterise whatever is built right now. One arm, no comparison.
//   Trace.Arena.PerfAB  - REBUILD the arena as legacy, sample it, rebuild it as instanced, sample
//                         that, N times over, then print both with the delta.
//
// WHY THE A/B IS IN ONE PROCESS. The obvious way to get a before and an after is two launches with
// and without -TraceArenaLegacyGeometry. That was tried first and it produced two numbers that cannot
// be compared: this machine is shared, and BOTH attempts had another headless UE run start up inside
// the 12-second sample window (once mid-warmup, once at second 3), which on a GPU-bound measurement
// is not noise, it is a second renderer. Interleaving the arms in one process spreads any such
// interference - and any thermal or match-state drift - evenly across both arms instead of dumping
// it on whichever one was unlucky. That is the same reasoning, and the same shape, as
// Trace.Trail.PerfAB's cycle count.
//
// r.ScreenPercentage 300 IS NOT OPTIONAL AND IS NOT DECORATION. macOS Metal paces the present to the
// display whatever t.MaxFPS, r.VSync, rhi.SyncInterval and -RenderOffScreen are set to. A run that
// lands on the refresh period has measured the CAP, and a capped frame time is identical for an arena
// that draws 1450 meshes and one that draws 40 - which is exactly how a previous pass concluded it
// had fixed something it had not. Making the GPU the bottleneck is what lets the comparison detect
// anything at all, and this harness refuses to let a capped table be read at face value: see the
// validity verdict printed under it.
//
//   ... -TraceAutoPlay=2 -TraceExec="r.ScreenPercentage 300|Trace.Arena.PerfAB 10 2 3" -TraceExecAt=14
// =================================================================================================

namespace
{
	/** Everything the report needs about the scene, gathered per arm so it can be read months later. */
	struct FArenaPerfCensus
	{
		int32 ScenePrimitives = 0;       // every registered primitive in the world, arena or not
		int32 ArenaPrimitives = 0;       // primitives owned by the arena builder
		int32 ArenaMovable = 0;          // ...of which Movable
		int32 ArenaShadowCasters = 0;    // ...of which cast a shadow
		int32 ArenaMeshComponents = 0;   // plain UStaticMeshComponents (the legacy arm's blocks)
		int32 ArenaInstancePools = 0;    // pooled ISMs (the instanced arm's blocks)
		int32 ArenaInstances = 0;        // total instances across those pools
		int32 ArenaBoxes = 0;            // collision boxes and pawn standoff shells
		int32 Characters = 0;
	};

	UWorld* FindArenaPerfWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
				&& Context.World() != nullptr)
			{
				return Context.World();
			}
		}

		return nullptr;
	}

	void GatherArenaPerfCensus(UWorld* World, FArenaPerfCensus& Out)
	{
		Out = FArenaPerfCensus();
		if (World == nullptr)
		{
			return;
		}

		for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
		{
			UPrimitiveComponent* Primitive = *It;
			if (Primitive == nullptr || Primitive->GetWorld() != World || !Primitive->IsRegistered())
			{
				continue;
			}

			++Out.ScenePrimitives;

			if (Primitive->GetOwner() == nullptr || !Primitive->GetOwner()->IsA<ATraceArenaBuilder>())
			{
				continue;
			}

			++Out.ArenaPrimitives;
			Out.ArenaMovable += (Primitive->Mobility == EComponentMobility::Movable) ? 1 : 0;
			Out.ArenaShadowCasters += Primitive->CastShadow ? 1 : 0;

			// ISM before StaticMesh: UInstancedStaticMeshComponent DERIVES from UStaticMeshComponent,
			// so testing the base class first would count every pool as a loose block and report the
			// instanced arm as though nothing had changed.
			if (const UInstancedStaticMeshComponent* Pool = Cast<UInstancedStaticMeshComponent>(Primitive))
			{
				++Out.ArenaInstancePools;
				Out.ArenaInstances += Pool->GetInstanceCount();
			}
			else if (Primitive->IsA<UStaticMeshComponent>())
			{
				++Out.ArenaMeshComponents;
			}
			else if (Primitive->IsA<UBoxComponent>())
			{
				++Out.ArenaBoxes;
			}
		}

		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			++Out.Characters;
		}
	}

	struct FArenaPerfArm
	{
		/** Trace.Arena.Instancing value this arm builds the arena with. */
		int32 Instancing = 1;
		FString Name;

		/**
		 * The arm every other arm's delta is quoted against. Exactly one arm should set it.
		 *
		 * It used to be inferred from Instancing == 0, which was true while the only A/B in this file
		 * was instanced-vs-legacy geometry and became wrong the moment spec v11 §3 added arms that all
		 * share one geometry path and differ only in which renderer feature is armed.
		 */
		bool bIsBaseline = false;

		/**
		 * Puts the renderer into this arm's configuration. Run at the top of the arm, before the
		 * warmup, so nothing it changes is measured until it has settled.
		 *
		 * This is the fidelity A/B's whole mechanism: the arena is NOT rebuilt between these arms (the
		 * geometry is identical in all of them), so an arm is a handful of console-variable writes and
		 * one ApplyFidelity call. That is what makes eight arms x three cycles affordable in the one
		 * process an uncontaminated measurement has to happen in.
		 */
		TFunction<void()> ApplyArm;

		TArray<float> FrameMs;
		double GameMsSum = 0.0;
		double RenderMsSum = 0.0;
		double GpuMsSum = 0.0;
		int32 Frames = 0;

		FArenaPerfCensus Census;
	};

	struct FArenaPerfState
	{
		TArray<FArenaPerfArm> Arms;
		int32 ArmIndex = 0;
		int32 CyclesTotal = 3;
		int32 CycleIndex = 0;

		float WarmupSeconds = 2.f;
		float SampleSeconds = 10.f;
		float PhaseElapsed = 0.f;
		bool bWarming = true;
		bool bStarted = false;

		/** false for Trace.Arena.Perf: sample what is already built, never rebuild anything. */
		bool bRebuildBetweenArms = true;

		int32 RestoreInstancing = 1;

		/**
		 * Run once when the last cycle ends, before the report is printed.
		 *
		 * An A/B harness that leaves the renderer in whatever the LAST arm happened to be is a trap:
		 * the fidelity run's last arm is "low", so without this every screenshot and every command
		 * typed after a run would silently be measuring a Low-preset arena. The instancing A/B has
		 * always restored itself (RestoreInstancing above); this is the same guarantee for the arms
		 * that are configuration rather than geometry.
		 */
		TFunction<void()> OnFinished;
	};

	float ArenaPerfMean(const TArray<float>& Values)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		double Total = 0.0;
		for (const float Value : Values)
		{
			Total += Value;
		}
		return static_cast<float>(Total / Values.Num());
	}

	float ArenaPerfPercentile(TArray<float>& Values, float Fraction)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		Values.Sort();
		const int32 Index = FMath::Clamp(FMath::RoundToInt(Fraction * (Values.Num() - 1)), 0, Values.Num() - 1);
		return Values[Index];
	}

	/**
	 * The refresh rate this run was pinned to, or 0 if it was free.
	 *
	 * Two halves, exactly as Trace.Trail.PerfAB's version has: the arms have to AGREE to within a
	 * couple of percent (a genuine tie is possible, so agreement alone must never be enough to cry
	 * cap) AND that shared mean has to sit on a plausible refresh period. A single-arm run degrades to
	 * the second half, which is the half that carries the information anyway - a measurement sitting
	 * within 3% of a refresh period is a measurement of the display.
	 */
	float ArenaPerfDetectFrameRateCap(const FArenaPerfState& State)
	{
		float MinMean = TNumericLimits<float>::Max();
		float MaxMean = 0.f;
		for (const FArenaPerfArm& Arm : State.Arms)
		{
			const float Mean = ArenaPerfMean(Arm.FrameMs);
			if (Mean <= 0.f)
			{
				return 0.f;   // An arm with no frames: nothing to conclude either way.
			}
			MinMean = FMath::Min(MinMean, Mean);
			MaxMean = FMath::Max(MaxMean, Mean);
		}

		if (MinMean <= 0.f || (State.Arms.Num() > 1 && ((MaxMean - MinMean) / MinMean) > 0.02f))
		{
			return 0.f;   // The arms genuinely differ, so nothing is holding them together.
		}

		static const float CommonRefreshRates[] = { 30.f, 50.f, 60.f, 72.f, 75.f, 90.f, 100.f, 120.f, 144.f, 165.f, 240.f };
		for (const float Hz : CommonRefreshRates)
		{
			const float Period = 1000.f / Hz;
			if (FMath::Abs(MinMean - Period) / Period <= 0.03f)
			{
				return Hz;
			}
		}

		return 0.f;
	}

	void ArenaPerfReport(FArenaPerfState& State)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ARENAPERF] ===== RESULTS. %d interleaved cycle(s) x %d arm(s) x %.1fs sample. ")
			TEXT("ms lower is better; fps from the MEAN frame time. ====="),
			State.CyclesTotal, State.Arms.Num(), State.SampleSeconds);

		for (const FArenaPerfArm& Arm : State.Arms)
		{
			const FArenaPerfCensus& C = Arm.Census;
			UE_LOG(LogTraceGame, Display,
				TEXT("[ARENAPERF] arm '%s' scene: arena owns %d registered primitives (%d Movable, %d shadow ")
				TEXT("casters) = %d loose static meshes + %d pooled ISMs carrying %d instances + %d ")
				TEXT("collision/standoff boxes. Whole world: %d primitives, %d characters."),
				*Arm.Name, C.ArenaPrimitives, C.ArenaMovable, C.ArenaShadowCasters,
				C.ArenaMeshComponents, C.ArenaInstancePools, C.ArenaInstances, C.ArenaBoxes,
				C.ScenePrimitives, C.Characters);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ARENAPERF] %-10s %6s | %8s %8s %8s | %8s %8s %8s | %7s"),
			TEXT("arm"), TEXT("frames"), TEXT("frameMs"), TEXT("medMs"), TEXT("p95Ms"),
			TEXT("gameMs"), TEXT("rendMs"), TEXT("gpuMs"), TEXT("fps"));

		float BaselineFrame = 0.f;
		FString BaselineName = TEXT("legacy");
		for (FArenaPerfArm& Arm : State.Arms)
		{
			const int32 SafeFrames = FMath::Max(1, Arm.Frames);
			const float MeanFrame = ArenaPerfMean(Arm.FrameMs);

			if (Arm.bIsBaseline)
			{
				BaselineFrame = MeanFrame;
				BaselineName = Arm.Name;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[ARENAPERF] %-10s %6d | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %7.1f"),
				*Arm.Name, Arm.Frames, MeanFrame,
				ArenaPerfPercentile(Arm.FrameMs, 0.50f), ArenaPerfPercentile(Arm.FrameMs, 0.95f),
				Arm.GameMsSum / SafeFrames, Arm.RenderMsSum / SafeFrames, Arm.GpuMsSum / SafeFrames,
				(MeanFrame > 0.f) ? (1000.f / MeanFrame) : 0.f);
		}

		if (BaselineFrame > 0.f)
		{
			for (FArenaPerfArm& Arm : State.Arms)
			{
				if (Arm.bIsBaseline)
				{
					continue;
				}
				const float MeanFrame = ArenaPerfMean(Arm.FrameMs);
				UE_LOG(LogTraceGame, Display,
					TEXT("[ARENAPERF] '%s' vs '%s': %+.2f ms/frame (%+.1f%%), %.2fx the frame rate."),
					*Arm.Name, *BaselineName, MeanFrame - BaselineFrame,
					100.f * (MeanFrame - BaselineFrame) / BaselineFrame,
					(MeanFrame > 0.f) ? (BaselineFrame / MeanFrame) : 0.f);
			}
		}

		// THE VALIDITY VERDICT, PRINTED LAST SO IT IS THE LINE UNDER THE TABLE.
		if (const float CapHz = ArenaPerfDetectFrameRateCap(State); CapHz > 0.f)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ARENAPERF] *** THIS MEASUREMENT IS INVALID: every arm landed within 2%% of %.2f ms, ")
				TEXT("which is %.0f Hz. The frame is PACED TO THE DISPLAY, not to the renderer, so it reads ")
				TEXT("the same whether the arena submits 1450 draws or 40 and the comparison cannot detect a ")
				TEXT("difference of any size. t.MaxFPS / r.VSync / rhi.SyncInterval do NOT defeat this on ")
				TEXT("macOS Metal, and -RenderOffScreen still presents. Run `r.ScreenPercentage 300` first so ")
				TEXT("the GPU is the bottleneck, and do NOT quote the numbers above. ***"),
				1000.f / CapHz, CapHz);
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[ARENAPERF] Validity: the arms differ by more than the cap-detection tolerance and the ")
				TEXT("shared mean is not a display refresh period, so the frame was NOT paced and this ")
				TEXT("comparison could have detected a difference."));
		}

		UE_LOG(LogTraceGame, Display, TEXT("[ARENAPERF] ===== END. ====="));
	}

	void ArenaPerfBeginArm(FArenaPerfState& State, UWorld* World)
	{
		FArenaPerfArm& Arm = State.Arms[State.ArmIndex];

		if (State.bRebuildBetweenArms)
		{
			// The arm's geometry path is latched by BuildArena, so the cvar has to be set BEFORE the
			// rebuild and not after it.
			GArenaInstancing = Arm.Instancing;

			int32 Rebuilt = 0;
			for (TActorIterator<ATraceArenaBuilder> It(World); It; ++It)
			{
				It->RebuildForMeasurement();
				++Rebuilt;
			}

			if (Rebuilt == 0)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ARENAPERF] no ATraceArenaBuilder in this world; the arms will all measure the same scene."));
			}
		}

		// The fidelity arms (spec v11 §3) change no geometry, only which renderer features are armed.
		// Run BEFORE the warmup so that whatever the change costs to set up - Lumen building a scene
		// representation, an FFT kernel being transformed - happens inside the warmup and not inside
		// the sample.
		if (Arm.ApplyArm)
		{
			Arm.ApplyArm();
		}

		GatherArenaPerfCensus(World, Arm.Census);

		State.bWarming = true;
		State.PhaseElapsed = 0.f;

		FString Configuration = FString::Printf(TEXT("Trace.Arena.Instancing %d"), Arm.Instancing);
		if (const ATraceArenaBuilder* Builder = ATraceArenaBuilder::Get(World))
		{
			Configuration = Builder->DescribeFidelity();
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ARENAPERF] arm '%s' (%s): %d arena primitives. Warming %.1fs, then sampling %.1fs."),
			*Arm.Name, *Configuration, Arm.Census.ArenaPrimitives, State.WarmupSeconds, State.SampleSeconds);
	}

	bool TickArenaPerf(FArenaPerfState& State, float DeltaTime)
	{
		UWorld* World = FindArenaPerfWorld();
		if (World == nullptr)
		{
			return true;   // The map may still be loading.
		}

		if (!State.bStarted)
		{
			State.bStarted = true;

			if (GEngine != nullptr)
			{
				// FStatUnitData is only filled while the overlay is enabled, so enable it here rather
				// than asking the caller to remember: a run that forgets produces zeroes in three
				// columns, which looks like a measurement and is not one.
				GEngine->Exec(World, TEXT("stat unit"));

				// Every cap the engine owns. None of them beats Metal's present pacing on macOS -
				// that is what r.ScreenPercentage is for - but leaving any of them on guarantees a
				// capped number even on hardware where they do work.
				GEngine->Exec(World, TEXT("t.MaxFPS 0"));
				GEngine->Exec(World, TEXT("r.VSync 0"));
				GEngine->Exec(World, TEXT("rhi.SyncInterval 0"));
				GEngine->bSmoothFrameRate = false;
				GEngine->bUseFixedFrameRate = false;
			}

			ArenaPerfBeginArm(State, World);
			return true;
		}

		State.PhaseElapsed += DeltaTime;

		if (State.bWarming)
		{
			if (State.PhaseElapsed >= State.WarmupSeconds)
			{
				State.bWarming = false;
				State.PhaseElapsed = 0.f;
			}
			return true;
		}

		FArenaPerfArm& Arm = State.Arms[State.ArmIndex];
		Arm.FrameMs.Add(DeltaTime * 1000.f);

		if (GEngine != nullptr && GEngine->GameViewport != nullptr)
		{
			if (const FStatUnitData* Unit = GEngine->GameViewport->GetStatUnitData())
			{
				Arm.GameMsSum += Unit->RawGameThreadTime;
				Arm.RenderMsSum += Unit->RawRenderThreadTime;
				Arm.GpuMsSum += Unit->RawGPUFrameTime[0];
			}
		}

		++Arm.Frames;

		if (State.PhaseElapsed < State.SampleSeconds)
		{
			return true;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ARENAPERF] arm '%s' done: %d frames, mean %.2f ms (%.1f fps)."),
			*Arm.Name, Arm.Frames, ArenaPerfMean(Arm.FrameMs),
			(ArenaPerfMean(Arm.FrameMs) > 0.f) ? (1000.f / ArenaPerfMean(Arm.FrameMs)) : 0.f);

		++State.ArmIndex;
		if (State.ArmIndex >= State.Arms.Num())
		{
			State.ArmIndex = 0;
			++State.CycleIndex;

			if (State.CycleIndex >= State.CyclesTotal)
			{
				// Leave the arena in the arm the project actually ships, whatever the last one
				// sampled happened to be.
				if (State.bRebuildBetweenArms)
				{
					GArenaInstancing = State.RestoreInstancing;
					for (TActorIterator<ATraceArenaBuilder> It(World); It; ++It)
					{
						It->RebuildForMeasurement();
					}
				}

				if (State.OnFinished)
				{
					State.OnFinished();
				}

				ArenaPerfReport(State);
				return false;
			}
		}

		ArenaPerfBeginArm(State, World);
		return true;
	}

	void StartArenaPerf(FArenaPerfState& State)
	{
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
			{
				return TickArenaPerf(State, DeltaTime);
			}), 0.f);
	}

	FAutoConsoleCommand CmdArenaPerf(
		TEXT("Trace.Arena.Perf"),
		TEXT("Trace.Arena.Perf [SampleSeconds] [WarmupSeconds] - census the arena's primitives and sample "
		     "frame / game / render / GPU milliseconds for whatever is built RIGHT NOW, then print the table "
		     "(spec v7 8). Nothing is rebuilt. Run `r.ScreenPercentage 300` first or the number you get is "
		     "the display's refresh rate."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FArenaPerfState State;
			State.SampleSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 2.f, 120.f) : 12.f;
			State.WarmupSeconds = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 0.5f, 30.f) : 3.f;
			State.CyclesTotal = 1;
			State.bRebuildBetweenArms = false;

			FArenaPerfArm Current;
			Current.Instancing = GArenaInstancing;
			Current.Name = TEXT("as-built");
			State.Arms.Add(MoveTemp(Current));

			UE_LOG(LogTraceGame, Display, TEXT("[ARENAPERF] starting: %.1fs warmup + %.1fs sample, no rebuild."),
				State.WarmupSeconds, State.SampleSeconds);
			StartArenaPerf(State);
		}));

	// ---------------------------------------------------------------------------------------------
	// SPEC v9 SECTION 10 - THE CROSS-SECTION HARNESS.
	//
	// Spec v9 section 0: reproduce the symptom, SEE IT FAIL, then fix it, then show the same
	// reproduction passing. The symptom here is a shape - "the crosssection of the arena" - so the
	// reproduction has to be a measurement of the shape, taken from a running game through the same
	// collision the pawns walk on, not a screenshot somebody squints at.
	//
	// Four cuts, each a line of downward traces marching in from a wall face, chosen to land clear of
	// the buttress row, the corner pylons and the gate towers so that what comes back is the GROUND
	// and not a piece of furniture standing on it. What each cut answers:
	//
	//   blend    ground height at the closest a pawn can stand (the 40 uu standoff line). This is the
	//            number that says whether the wall comes out of the floor square. Zero is a 90 degree
	//            join with the full 2600 uu of wall rising straight out of a flat floor.
	//   toe      how far out the transition starts, i.e. how long the curve is.
	//   riser    the largest vertical jump between adjacent samples. Over MaxStepHeight (45) and the
	//            curve is an unclimbable wall; this is the walkability test.
	//   steps    how many distinct levels the transition is made of. One is a kerb, and a staircase
	//            of two or three reads as a staircase; it takes six or seven before the eye stops
	//            counting them and starts seeing a curve.
	//
	// Traced on ECC_Visibility rather than ECC_Pawn on purpose: the pawn-only standoff shells are
	// vertical planes 40 uu off each wall running the full height, so a pawn-channel probe inside one
	// reports its lid at 2600 uu and the profile becomes nonsense. Visibility sees the real geometry,
	// and the standoff line is reported separately as the closest a body may stand.
	// ---------------------------------------------------------------------------------------------

	struct FTraceWallCut
	{
		FString Name;
		FVector Foot;      // world, on the wall's inner face, at floor level
		FVector Inward;    // unit, pointing into the field
	};

	void ReportArenaCrossSection(float Reach, float SampleStep)
	{
		UWorld* World = FindArenaPerfWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[XSECTION] no game world."));
			return;
		}

		ATraceArenaBuilder* Builder = ATraceArenaBuilder::Get(World);
		if (Builder == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[XSECTION] no ATraceArenaBuilder in this world."));
			return;
		}

		const FBox Field = Builder->GetFieldBounds();
		const float HalfX = Field.GetExtent().X;
		const float HalfY = Field.GetExtent().Y;
		const FVector Mid = Field.GetCenter();
		const float FloorZ = Field.Min.Z;

		// 800 uu up: above anything the cove could ever be and below the flank rail (1640) and the
		// light bridges (1240), so a cut never lands on a beam and calls it the ground.
		const float ProbeTopZ = FloorZ + 800.f;

		TArray<FTraceWallCut> Cuts;
		// Midfield side wall: X = 0.075 * HalfX, i.e. 1050 uu clear of the nearest buttress either way.
		Cuts.Add({ TEXT("side wall, midfield"),
			FVector(Mid.X + HalfX * 0.075f, Mid.Y + HalfY, FloorZ), FVector(0.f, -1.f, 0.f) });
		// Side wall inside the endzone, 600 uu past the goal line: clear of the gate towers (on the
		// line) and of the corner pylons (mid endzone), and outside the end wall's own cove.
		Cuts.Add({ TEXT("side wall, endzone"),
			FVector(Mid.X + HalfX - 1800.f, Mid.Y + HalfY, FloorZ), FVector(0.f, -1.f, 0.f) });
		// End wall down the scoring lane - the one cut that has to prove the cove stays UNDER the
		// mode-B carry-in ramp rather than putting a lip across the mouth of the hoop.
		Cuts.Add({ TEXT("end wall, goal lane"),
			FVector(Mid.X + HalfX, Mid.Y, FloorZ), FVector(-1.f, 0.f, 0.f) });
		// End wall off the lane, between two end buttresses.
		Cuts.Add({ TEXT("end wall, off lane"),
			FVector(Mid.X + HalfX, Mid.Y + HalfY * 0.65f, FloorZ), FVector(-1.f, 0.f, 0.f) });

		FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceArenaCrossSection), /*bTraceComplex=*/false);
		Params.bFindInitialOverlaps = false;

		UE_LOG(LogTraceGame, Display,
			TEXT("[XSECTION] ===== arena cross-section, %.0f uu in from each wall face in %.0f uu steps. ")
			TEXT("Ground height measured on ECC_Visibility. ====="), Reach, SampleStep);

		for (const FTraceWallCut& Cut : Cuts)
		{
			const int32 SampleCount = FMath::Clamp(FMath::CeilToInt(Reach / FMath::Max(1.f, SampleStep)), 2, 512);

			TArray<float> Ground;
			TArray<float> Distance;
			Ground.Reserve(SampleCount);
			Distance.Reserve(SampleCount);

			for (int32 Index = 0; Index < SampleCount; ++Index)
			{
				// From 5 uu off the face outward. Never 0: a probe started exactly on a wall face is a
				// coin toss between the wall and the air beside it.
				const float D = 5.f + SampleStep * static_cast<float>(Index);
				const FVector Column = Cut.Foot + Cut.Inward * D;

				FHitResult Hit;
				const bool bHit = World->LineTraceSingleByChannel(Hit,
					FVector(Column.X, Column.Y, ProbeTopZ), FVector(Column.X, Column.Y, FloorZ - 200.f),
					ECC_Visibility, Params);

				Distance.Add(D);
				Ground.Add(bHit ? (Hit.ImpactPoint.Z - FloorZ) : -999.f);
			}

			// Walk OUTWARD to INWARD, which is the direction a player climbs, so a "riser" is a step up.
			float MaxRiser = 0.f;
			float MaxRiserAt = 0.f;
			float Toe = -1.f;
			int32 Levels = 0;
			float LastLevel = -1000.f;

			for (int32 Index = SampleCount - 1; Index >= 0; --Index)
			{
				const float Here = Ground[Index];
				if (Here < -500.f)
				{
					continue;
				}

				if (Index < SampleCount - 1 && Ground[Index + 1] > -500.f)
				{
					const float Rise = Here - Ground[Index + 1];
					if (Rise > MaxRiser)
					{
						MaxRiser = Rise;
						MaxRiserAt = Distance[Index];
					}
				}

				if (Here > 1.f && Toe < 0.f)
				{
					Toe = Distance[Index];
				}

				if (FMath::Abs(Here - LastLevel) > 1.f)
				{
					++Levels;
					LastLevel = Here;
				}
			}

			const float Blend = Ground.Num() > 0 ? Ground[0] : 0.f;

			UE_LOG(LogTraceGame, Display,
				TEXT("[XSECTION] %-22s blend %6.1f uu at the wall | toe %6.0f uu out | max riser %5.1f uu ")
				TEXT("(at %.0f uu, ceiling 45) | %d levels"),
				*Cut.Name, Blend, (Toe >= 0.f) ? Toe : 0.f, MaxRiser, MaxRiserAt, Levels);

			// The shape itself, drawn. One row per 40 uu of height so the log carries the actual
			// cross-section rather than four numbers that describe one.
			FString Profile;
			for (int32 Index = 0; Index < SampleCount; ++Index)
			{
				Profile += FString::Printf(TEXT("%.0f:%.0f "), Distance[Index], FMath::Max(0.f, Ground[Index]));
			}
			UE_LOG(LogTraceGame, Display, TEXT("[XSECTION]   %s -> %s"), *Cut.Name, *Profile);
		}

		UE_LOG(LogTraceGame, Display, TEXT("[XSECTION] ===== END. ====="));
	}

	FAutoConsoleCommand CmdArenaCrossSection(
		TEXT("Trace.Arena.CrossSection"),
		TEXT("Trace.Arena.CrossSection [Reach] [SampleStep] - march downward traces in from each wall face "
		     "and print the arena's cross-section: the ground height at the wall, how far out the "
		     "transition starts, the largest riser in it and how many levels it is made of (spec v9 10)."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Reach = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 100.f, 4000.f) : 1700.f;
			const float Step = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 5.f, 200.f) : 25.f;
			ReportArenaCrossSection(Reach, Step);
		}));

	// ---------------------------------------------------------------------------------------------
	// SPEC v9 SECTION 10 - "IT MUST BE WALKABLE ... confirm nobody gets stuck against the base of a
	// wall - bots steer directly, so a lip at the bottom of the curve will trap them."
	//
	// A pawn standing still against a wall is not evidence of anything: it might be dead, idle, or
	// holding an angle. WEDGED means something specific and it is what this measures - the pawn is
	// ASKING to move (non-trivial Acceleration, which is what AddMovementInput leaves behind, or a
	// requested velocity from path following), it is inside the band of floor the cove occupies, and
	// it is going nowhere. That triple is exactly the failure the instruction describes and it cannot
	// be produced by a bot that has simply stopped.
	// ---------------------------------------------------------------------------------------------

	struct FTraceWallStickState
	{
		float Remaining = 30.f;
		float Band = 900.f;

		int32 Samples = 0;
		int32 WedgedSamples = 0;
		int32 InBandSamples = 0;
		float WorstStreakSeconds = 0.f;
		float HighestGroundZ = 0.f;
		FString WorstPawn;

		TMap<FString, float> Streaks;
	};

	bool TickArenaWallStick(FTraceWallStickState& State, float DeltaTime)
	{
		UWorld* World = FindArenaPerfWorld();
		ATraceArenaBuilder* Builder = (World != nullptr) ? ATraceArenaBuilder::Get(World) : nullptr;
		if (World == nullptr || Builder == nullptr)
		{
			return true;   // Still loading.
		}

		const FBox Field = Builder->GetFieldBounds();
		const FVector Mid = Field.GetCenter();
		const FVector Extent = Field.GetExtent();

		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Character = *It;
			if (!IsValid(Character) || !Character->IsAlive())
			{
				continue;
			}

			const UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
			if (Movement == nullptr)
			{
				continue;
			}

			const FVector Where = Character->GetActorLocation();
			const float ToWallY = Extent.Y - FMath::Abs(Where.Y - Mid.Y);
			const float ToWallX = Extent.X - FMath::Abs(Where.X - Mid.X);
			const float ToWall = FMath::Min(ToWallX, ToWallY);

			++State.Samples;
			if (ToWall > State.Band)
			{
				State.Streaks.Remove(GetNameSafe(Character));
				continue;
			}

			++State.InBandSamples;
			State.HighestGroundZ = FMath::Max(State.HighestGroundZ, Where.Z - Field.Min.Z);

			// BOTH ways a pawn can be ASKING to move, because Trace has both in play. Trace's bots have
			// no navmesh and steer with AddMovementInput, which lands in GetCurrentAcceleration(); a
			// pawn under path following instead leaves its wish in the requested velocity. Read through
			// GetLastUpdateRequestedVelocity() rather than the RequestedVelocity field - that field and
			// bHasRequestedVelocity are PROTECTED on UCharacterMovementComponent, and the public
			// accessor already returns a zero vector on the frames path following did not request
			// anything, so the "has one" flag is not needed either.
			const float WishSize = FMath::Max(Movement->GetCurrentAcceleration().Size2D(),
				static_cast<float>(Movement->GetLastUpdateRequestedVelocity().Size2D()));
			const bool bWedged = (WishSize > 100.f) && (Movement->Velocity.Size2D() < 50.f);

			float& Streak = State.Streaks.FindOrAdd(GetNameSafe(Character));
			if (!bWedged)
			{
				Streak = 0.f;
				continue;
			}

			++State.WedgedSamples;
			Streak += DeltaTime;
			if (Streak > State.WorstStreakSeconds)
			{
				State.WorstStreakSeconds = Streak;
				State.WorstPawn = FString::Printf(TEXT("%s at (%.0f, %.0f, %.0f), %.0f uu off a wall"),
					*GetNameSafe(Character), Where.X, Where.Y, Where.Z, ToWall);
			}
		}

		State.Remaining -= DeltaTime;
		if (State.Remaining > 0.f)
		{
			return true;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[WALLSTICK] ===== %d pawn-samples, %d of them inside %.0f uu of a wall. Wedged (wants to ")
			TEXT("move, is not moving): %d samples = %.2f%% of the in-band ones. Worst continuous wedge ")
			TEXT("%.2f s%s%s. Highest a pawn stood near a wall: %.0f uu. ====="),
			State.Samples, State.InBandSamples, State.Band,
			State.WedgedSamples,
			(State.InBandSamples > 0) ? (100.f * State.WedgedSamples / State.InBandSamples) : 0.f,
			State.WorstStreakSeconds,
			State.WorstPawn.IsEmpty() ? TEXT("") : TEXT(" - "),
			State.WorstPawn.IsEmpty() ? TEXT("") : *State.WorstPawn,
			State.HighestGroundZ);

		return false;
	}

	FAutoConsoleCommand CmdArenaWallStick(
		TEXT("Trace.Arena.WallStick"),
		TEXT("Trace.Arena.WallStick [Seconds] [BandUU] - watch every living pawn inside BandUU of a wall and "
		     "report how often one is asking to move and going nowhere, i.e. caught on the base of the wall "
		     "cove (spec v9 10)."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FTraceWallStickState State;
			State.Remaining = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 2.f, 600.f) : 45.f;
			State.Band = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 100.f, 4000.f) : 900.f;

			UE_LOG(LogTraceGame, Display, TEXT("[WALLSTICK] watching for %.0fs within %.0f uu of a wall."),
				State.Remaining, State.Band);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickArenaWallStick(State, DeltaTime);
				}), 0.f);
		}));

	// ---------------------------------------------------------------------------------------------
	// SPEC v9 SECTION 10 - "IT MUST BE WALKABLE. Players and bots should ride up it rather than catch
	// on it."
	//
	// WHY Trace.Arena.WallStick IS NOT ENOUGH ON ITS OWN. It watches whoever happens to be near a
	// wall, and measured over two 100 s ten-bot matches, one per arm, NOBODY EVER IS: 11288 and 5344
	// pawn-samples, 0 of them inside 900 uu of a wall, in the square arm as well as the coved one.
	// Trace's bots play the middle of the field. A harness with no exposure to the geometry it is
	// checking cannot go red, and spec v9 section 0 is a post-mortem on exactly that mistake.
	//
	// So this creates the exposure. It walks the local pawn STRAIGHT INTO the base of a wall and
	// reports how far up it got, which is the question stated as a number.
	//
	// It steers with AddMovementInput and nothing else - the same call ATraceBotController uses, and
	// the reason the instruction singles bots out ("bots steer directly, so a lip at the bottom of the
	// curve will trap them"). Nothing here writes Velocity, the movement mode or the location after
	// the initial placement, so the climb is UCharacterMovementComponent's own step-up logic or it
	// does not happen at all.
	//
	// THE TWO ARMS DIFFER IN ONE NUMBER, the feet height reached at the wall:
	//   -TraceArenaSquareCorners   a vertical face at the floor: the pawn stops dead at 0 uu.
	//   default                    the cove: the pawn rides it up to the top tread.
	// A lip anywhere in the curve shows up as a stall PART WAY UP - the report carries the height and
	// the distance the worst stall happened at, so "it caught" and "it arrived" are different numbers
	// rather than the same pass/fail.
	// ---------------------------------------------------------------------------------------------

	struct FTraceCoveWalkState
	{
		float Remaining = 20.f;
		/** 0 = walk at the +X end wall, 1 = walk at the +Y side wall. */
		int32 Axis = 1;

		bool bPlaced = false;
		float Elapsed = 0.f;

		float StartDistance = 0.f;
		float ClosestApproach = TNumericLimits<float>::Max();
		float BestFeetZ = 0.f;
		float FinalFeetZ = 0.f;

		float StallSeconds = 0.f;
		float WorstStall = 0.f;
		float WorstStallDistance = -1.f;
		float WorstStallFeetZ = -1.f;
	};

	bool TickArenaCoveWalk(FTraceCoveWalkState& State, float DeltaTime)
	{
		UWorld* World = FindArenaPerfWorld();
		ATraceArenaBuilder* Builder = (World != nullptr) ? ATraceArenaBuilder::Get(World) : nullptr;
		if (World == nullptr || Builder == nullptr)
		{
			return true;   // Still loading.
		}

		// Deliberately the LOCAL player rather than a bot: a bot is being steered by its own
		// controller every frame and the two inputs would fight. Named TestPawn/TestController
		// because a local called Pawn or Controller shadows a member on APawn/AController on MSVC,
		// which clang does not warn about and Windows refuses to compile.
		APlayerController* TestController = World->GetFirstPlayerController();
		ACharacter* TestPawn = (TestController != nullptr) ? Cast<ACharacter>(TestController->GetPawn()) : nullptr;
		if (TestPawn == nullptr || TestPawn->GetCharacterMovement() == nullptr)
		{
			return true;   // No pawn possessed yet.
		}

		const FBox Field = Builder->GetFieldBounds();
		const FVector Mid = Field.GetCenter();
		const FVector Extent = Field.GetExtent();
		const float FloorZ = Field.Min.Z;
		const float HalfHeight = TestPawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

		// Straight at the wall, on the axis asked for.
		//
		// THE START COLUMN IS THE ONE THE CROSS-SECTION ALREADY PROBES, and picking it by hand is how
		// the first attempt was wasted: a column 1800 uu in from the +X end wall runs through a corner
		// pylon, and both arms reported the pawn stalling 930 uu short of the wall on the pylon rather
		// than anywhere near the cove. These two are the "side wall, midfield" and "end wall, off lane"
		// cuts of Trace.Arena.CrossSection, which are chosen to miss the buttresses, the pylons and the
		// goal furniture and are printed alongside this so the two measurements are of the same ground.
		const FVector Direction = (State.Axis == 0) ? FVector(1.f, 0.f, 0.f) : FVector(0.f, 1.f, 0.f);
		const FRotator Facing(0.f, (State.Axis == 0) ? 0.f : 90.f, 0.f);

		// Far enough out to start on FLAT floor in both arms: the corner bank reaches BankDepth
		// (1500 uu) in from the sideline, so 1900 begins outside everything and the walk has to cross
		// the whole transition, bank and cove together, exactly as a bot running at the sideline would.
		const FVector Start = (State.Axis == 0)
			? FVector(Mid.X + Extent.X - 1900.f, Mid.Y + Extent.Y * 0.65f, FloorZ + HalfHeight + 20.f)
			: FVector(Mid.X + Extent.X * 0.075f, Mid.Y + Extent.Y - 1900.f, FloorZ + HalfHeight + 20.f);

		// RESPAWNS PUT THE PAWN BACK ON A SPAWN PAD, and a kickoff during the walk did exactly that in
		// the first run - the report ended up describing a stall 5429 uu from the wall, i.e. a pawn
		// that was no longer anywhere near the test. Anything that moves the pawn off the walk line
		// re-places it and restarts the clock rather than quietly polluting the numbers.
		const float LateralOffset = (State.Axis == 0)
			? FMath::Abs(TestPawn->GetActorLocation().Y - Start.Y)
			: FMath::Abs(TestPawn->GetActorLocation().X - Start.X);

		if (State.bPlaced && LateralOffset > 600.f)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[COVEWALK] pawn left the walk line (%.0f uu off it, probably a respawn) - re-placing."),
				LateralOffset);
			State.bPlaced = false;
			State.Elapsed = 0.f;
			State.StallSeconds = 0.f;
			State.WorstStall = 0.f;
			State.ClosestApproach = TNumericLimits<float>::Max();
			State.BestFeetZ = 0.f;
		}

		if (!State.bPlaced)
		{
			if (!TestPawn->TeleportTo(Start, Facing))
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[COVEWALK] could not place the pawn at %s."), *Start.ToCompactString());
				return false;
			}

			TestController->SetControlRotation(Facing);
			State.bPlaced = true;
			State.StartDistance = (State.Axis == 0)
				? (Extent.X - FMath::Abs(Start.X - Mid.X))
				: (Extent.Y - FMath::Abs(Start.Y - Mid.Y));

			UE_LOG(LogTraceGame, Display,
				TEXT("[COVEWALK] %s pawn placed at %s, %.0f uu from the %s wall, walking straight at it for %.0fs."),
				*GetNameSafe(TestPawn), *Start.ToCompactString(), State.StartDistance,
				(State.Axis == 0) ? TEXT("end") : TEXT("side"), State.Remaining);
		}

		// Hold the aim and the forward key. AddMovementInput only - see the block comment.
		TestController->SetControlRotation(Facing);
		TestPawn->AddMovementInput(Direction, 1.f);

		const FVector Here = TestPawn->GetActorLocation();
		const float Distance = (State.Axis == 0)
			? (Extent.X - FMath::Abs(Here.X - Mid.X))
			: (Extent.Y - FMath::Abs(Here.Y - Mid.Y));
		const float FeetZ = Here.Z - HalfHeight - FloorZ;

		State.Elapsed += DeltaTime;
		State.ClosestApproach = FMath::Min(State.ClosestApproach, Distance);
		State.BestFeetZ = FMath::Max(State.BestFeetZ, FeetZ);
		State.FinalFeetZ = FeetZ;

		// A stall only counts once the pawn has had a second to get moving, or the first frame after
		// the teleport would be recorded as one.
		const float Speed = TestPawn->GetCharacterMovement()->Velocity.Size2D();
		if (State.Elapsed > 1.f && Speed < 40.f)
		{
			State.StallSeconds += DeltaTime;
			if (State.StallSeconds > State.WorstStall)
			{
				State.WorstStall = State.StallSeconds;
				State.WorstStallDistance = Distance;
				State.WorstStallFeetZ = FeetZ;
			}
		}
		else
		{
			State.StallSeconds = 0.f;
		}

		State.Remaining -= DeltaTime;
		if (State.Remaining > 0.f)
		{
			return true;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[COVEWALK] ===== %s wall: walked in from %.0f uu, closest approach %.0f uu. FEET REACHED ")
			TEXT("%.0f uu (finished at %.0f uu). Longest stall %.2f s%s. ====="),
			(State.Axis == 0) ? TEXT("end") : TEXT("side"),
			State.StartDistance, State.ClosestApproach, State.BestFeetZ, State.FinalFeetZ, State.WorstStall,
			(State.WorstStall > 0.5f)
				? *FString::Printf(TEXT(" at %.0f uu out, feet %.0f uu"), State.WorstStallDistance, State.WorstStallFeetZ)
				: TEXT(""));

		return false;
	}

	FAutoConsoleCommand CmdArenaCoveWalk(
		TEXT("Trace.Arena.CoveWalk"),
		TEXT("Trace.Arena.CoveWalk [side|end] [Seconds] - teleport the local pawn out in front of a wall and "
		     "walk it straight into the base of that wall with AddMovementInput, then report how far up it "
		     "got and whether it ever stalled on the way (spec v9 10)."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FTraceCoveWalkState State;
			State.Axis = (Args.Num() > 0 && Args[0].Equals(TEXT("end"), ESearchCase::IgnoreCase)) ? 0 : 1;
			State.Remaining = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 2.f, 120.f) : 20.f;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickArenaCoveWalk(State, DeltaTime);
				}), 0.f);
		}));

	/**
	 * The camera, because the engine's BugItGo is a CheatManager exec and there is no cheat manager in
	 * a -game listen server ("Command not recognized: BugItGo", measured). Screenshotting the cove
	 * needs the view to be somewhere other than a spawn pad facing an endzone, and this is the whole
	 * of what that takes: put the pawn there and point it. Pair it with Trace.ForceThirdPerson 1.
	 */
	FAutoConsoleCommand CmdArenaPose(
		TEXT("Trace.Arena.Pose"),
		TEXT("Trace.Arena.Pose X Y Z [Pitch] [Yaw] - teleport the local pawn to a world position and aim it, "
		     "so the arena can be photographed from somewhere useful (spec v9 10)."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			if (Args.Num() < 3)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[POSE] usage: Trace.Arena.Pose X Y Z [Pitch] [Yaw]"));
				return;
			}

			UWorld* World = FindArenaPerfWorld();
			APlayerController* TestController = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
			APawn* TestPawn = (TestController != nullptr) ? TestController->GetPawn() : nullptr;
			if (TestPawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[POSE] no locally controlled pawn."));
				return;
			}

			const FVector Where(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2]));
			const FRotator Aim(
				(Args.Num() > 3) ? FCString::Atof(*Args[3]) : 0.f,
				(Args.Num() > 4) ? FCString::Atof(*Args[4]) : 0.f,
				0.f);

			const bool bMoved = TestPawn->TeleportTo(Where, Aim);
			TestController->SetControlRotation(Aim);

			UE_LOG(LogTraceGame, Display, TEXT("[POSE] %s -> %s aim %s (moved=%d)"),
				*GetNameSafe(TestPawn), *Where.ToCompactString(), *Aim.ToCompactString(), bMoved ? 1 : 0);
		}));

	FAutoConsoleCommand CmdArenaPerfAB(
		TEXT("Trace.Arena.PerfAB"),
		TEXT("Trace.Arena.PerfAB [SampleSeconds] [WarmupSeconds] [Cycles] - REBUILD the arena as legacy "
		     "one-component-per-block, sample it, rebuild it instanced, sample that, Cycles times "
		     "interleaved, then print the comparison (spec v7 8). Run `r.ScreenPercentage 300` first so the "
		     "GPU is the bottleneck, or the harness will tell you the result is the display refresh rate."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FArenaPerfState State;
			State.SampleSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 2.f, 120.f) : 10.f;
			State.WarmupSeconds = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 0.5f, 30.f) : 2.f;
			State.CyclesTotal = (Args.Num() > 2) ? FMath::Clamp(FCString::Atoi(*Args[2]), 1, 20) : 3;
			State.RestoreInstancing = GArenaInstancing;

			// LEGACY FIRST, deliberately, so the arm that allocates ~960 components is the one paying
			// for a cold heap rather than the one that is supposed to look good.
			FArenaPerfArm Legacy;
			Legacy.Instancing = 0;
			Legacy.Name = TEXT("legacy");
			Legacy.bIsBaseline = true;
			FArenaPerfArm Instanced;
			Instanced.Instancing = 1;
			Instanced.Name = TEXT("instanced");
			State.Arms.Add(MoveTemp(Legacy));
			State.Arms.Add(MoveTemp(Instanced));

			UE_LOG(LogTraceGame, Display,
				TEXT("[ARENAPERF] starting: %d interleaved cycles x 2 arms x (%.1fs warmup + %.1fs sample) = %.0fs total."),
				State.CyclesTotal, State.WarmupSeconds, State.SampleSeconds,
				State.CyclesTotal * 2.f * (State.WarmupSeconds + State.SampleSeconds));
			StartArenaPerf(State);
		}));

	// =============================================================================================
	// SPEC v11 §3/§4 - WHAT EACH FIDELITY FEATURE COSTS, ONE AT A TIME
	//
	// The request is not "is Epic slower than Low" - that is obvious and useless. It is "what does
	// each feature cost, so the user can decide what to keep", and the only way to answer that is one
	// arm per feature, all against a common baseline, interleaved in ONE process for the reason the
	// Trace.Arena.PerfAB header gives at length (this machine is shared; a second UE run starting up
	// inside a sample window is not noise, it is a second renderer).
	//
	// The arms below are ADDITIVE against a baseline that reproduces the shipped look exactly:
	// no GI, no AO, 3 shadow cascades, standard bloom, SSR quality 100, full-radius lamps. Each
	// "+feature" arm changes ONE group. "epic" and "low" are the two ends of the ladder as a player
	// would actually get them.
	//
	//   ... -TraceExec="r.ScreenPercentage 300|Trace.Arena.FidelityAB 4 1.5 2" -TraceExecAt=14
	// =============================================================================================

	/** Sets every fidelity override at once, then re-applies. INDEX_NONE (-1) means "follow scalability". */
	void SetArenaFidelityArm(int32 GI, int32 Reflections, int32 AO, int32 Shadows, int32 Bloom, int32 Lamps)
	{
		// Written through the cvar objects rather than the ints so the console reports the truth if a
		// human types `Trace.Arena.Fidelity.GI` mid-run, and so the change callbacks fire exactly once
		// each. ApplyFidelity is idempotent, so the repeated calls cost property writes and nothing.
		auto Push = [](const TCHAR* Name, int32 Value)
		{
			if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
			{
				CVar->Set(Value, ECVF_SetByCode);
			}
		};

		Push(TEXT("Trace.Arena.Fidelity.GI"), GI);
		Push(TEXT("Trace.Arena.Fidelity.Reflections"), Reflections);
		Push(TEXT("Trace.Arena.Fidelity.AO"), AO);
		Push(TEXT("Trace.Arena.Fidelity.Shadows"), Shadows);
		Push(TEXT("Trace.Arena.Fidelity.Bloom"), Bloom);
		Push(TEXT("Trace.Arena.Fidelity.Lamps"), Lamps);

		ApplyArenaFidelityEverywhere();
	}

	/** One arm of the fidelity A/B: a name and the six tiers it forces. */
	struct FArenaFidelityArmSpec
	{
		const TCHAR* Name;
		int32 GI;
		int32 Reflections;
		int32 AO;
		int32 Shadows;
		int32 Bloom;
		int32 Lamps;
		bool bBaseline;
	};

	// GI, Reflections, AO, Shadows, Bloom, Lamps.
	const FArenaFidelityArmSpec GArenaFidelityArms[] =
	{
		// The shipped look, and the thing every delta below is measured against.
		{ TEXT("shipped"),   0, 2, 0, 2, 2, 2, true  },
		// AO ON TOP OF LUMEN, not on top of the shipped look, and the change is deliberate.
		// ApplyFidelity now refuses to arm ambient occlusion unless Lumen GI is armed too, because
		// the measured standalone SSAO cost was +9.0% for a screenshot nobody can tell apart (see
		// the AO block). Against '+lumen' this arm therefore measures what AO costs where it is
		// actually reachable - which should be ~0, since Lumen replaces the SSAO pass outright.
		{ TEXT("+lumen+ao"), 3, 2, 3, 2, 2, 2, false },
		{ TEXT("+shadows"),  0, 2, 0, 3, 2, 2, false },
		{ TEXT("+fftbloom"), 0, 2, 0, 2, 3, 2, false },
		{ TEXT("+lumen"),    3, 2, 0, 2, 2, 2, false },
		{ TEXT("+lumenrefl"),3, 3, 0, 2, 2, 2, false },
		// THREE SUBTRACTIVE ARMS, and they are the half of this table that matters most for spec
		// v11 §0: what the SHIPPED look is already paying for the floor mirror, the shadow pass and
		// the lamp lattice. A cost you can only remove is still a cost, and these are the three
		// levers a struggling machine has that nobody in this project has ever measured.
		{ TEXT("-ssr"),      0, 0, 0, 2, 2, 2, false },
		{ TEXT("lamps-low"), 0, 2, 0, 2, 2, 0, false },
		{ TEXT("-shadows"),  0, 2, 0, 0, 2, 2, false },
		// The two ends of the ladder as a player gets them.
		{ TEXT("epic"),      3, 3, 3, 3, 3, 3, false },
		{ TEXT("low"),       0, 0, 0, 0, 0, 0, false },
		// THE NULL ARM, AND EVERY NUMBER IN THE TABLE SHOULD BE READ AGAINST IT.
		//
		// Byte for byte the same configuration as 'shipped', sampled at the far end of the cycle from
		// it. It measures nothing about the renderer and everything about the SCENE: this harness
		// samples a live 5v5 bot match from a first-person camera on a pawn that walks, dies and
		// respawns, so what is in front of the eye is different from one 4-second window to the next.
		//
		// MEASURED, first run, no null arm: three arms that were configured IDENTICALLY (Lumen was
		// being held off for want of distance fields, so 'shipped', '+lumen' and '+lumenrefl' were the
		// same renderer) read 40.68, 36.09 and 34.01 ms - a 16% spread with no cause. Without a null
		// arm that spread is invisible and every small delta in the table looks like a result. With
		// one, |control - shipped| is the floor under which nothing may be claimed, and raising the
		// cycle count until that floor is small is the whole of making this measurement trustworthy.
		{ TEXT("control"),   0, 2, 0, 2, 2, 2, false },
	};

	FAutoConsoleCommand CmdArenaFidelityAB(
		TEXT("Trace.Arena.FidelityAB"),
		TEXT("Trace.Arena.FidelityAB [SampleSeconds] [WarmupSeconds] [Cycles] - interleave one arm per "
		     "fidelity feature (spec v11 3) against the shipped configuration and print what each one "
		     "costs. The arena is NOT rebuilt: every arm is the same geometry with a different set of "
		     "renderer features armed. Run `r.ScreenPercentage 300` first so the GPU is the bottleneck, "
		     "or the harness will tell you the result is the display refresh rate."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FArenaPerfState State;
			State.SampleSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 2.f, 120.f) : 5.f;
			State.WarmupSeconds = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 0.5f, 30.f) : 2.f;
			State.CyclesTotal = (Args.Num() > 2) ? FMath::Clamp(FCString::Atoi(*Args[2]), 1, 20) : 2;

			// NOTHING IS REBUILT. The whole point of these arms is that the geometry is identical in
			// all of them, so any difference is the renderer feature and not a different arena.
			State.bRebuildBetweenArms = false;

			for (const FArenaFidelityArmSpec& Spec : GArenaFidelityArms)
			{
				FArenaPerfArm Arm;
				Arm.Name = Spec.Name;
				Arm.Instancing = GArenaInstancing;
				Arm.bIsBaseline = Spec.bBaseline;
				Arm.ApplyArm = [Spec]()
				{
					SetArenaFidelityArm(Spec.GI, Spec.Reflections, Spec.AO, Spec.Shadows, Spec.Bloom, Spec.Lamps);
				};
				State.Arms.Add(MoveTemp(Arm));
			}

			// Hand the renderer back to the video settings when the run ends - see OnFinished.
			State.OnFinished = []()
			{
				SetArenaFidelityArm(-1, -1, -1, -1, -1, -1);
			};

			UE_LOG(LogTraceGame, Display,
				TEXT("[ARENAPERF] fidelity A/B starting: %d interleaved cycles x %d arms x (%.1fs warmup + ")
				TEXT("%.1fs sample) = %.0fs total. r.GenerateMeshDistanceFields=%d (Lumen software tracing ")
				TEXT("needs 1)."),
				State.CyclesTotal, State.Arms.Num(), State.WarmupSeconds, State.SampleSeconds,
				State.CyclesTotal * State.Arms.Num() * (State.WarmupSeconds + State.SampleSeconds),
				ArenaMeshDistanceFieldsEnabled());

			StartArenaPerf(State);
		}));

	// =============================================================================================
	// Trace.Video.PresetAB - LOW / MEDIUM / HIGH / EPIC, through the path a player actually uses
	// =============================================================================================
	//
	// WHY THIS EXISTS ALONGSIDE Trace.Arena.FidelityAB, WHICH LOOKS LIKE THE SAME THING.
	//
	// FidelityAB drives the Trace.Arena.Fidelity.* OVERRIDE cvars. Those bypass scalability
	// entirely, which is exactly what you want when isolating one feature - and exactly what you
	// must not do when answering "does the LOW preset actually make this machine faster?". A preset
	// is far more than this file's six features: it is also the engine's own scalability tables
	// (view distance, TSR quality, shadow map resolution, texture streaming pool, effects LODs),
	// and if the ladder is measured through the overrides then every one of those is missing from
	// the number.
	//
	// So each arm here calls UTraceGameUserSettings::SetOverallQuality + ApplyVideoSettings - the
	// same two calls the VIDEO page's OVERALL QUALITY row makes - and nothing else. The fidelity
	// overrides are cleared first so that scalability, and therefore the menu, is genuinely what is
	// being measured. If this table shows no separation, the ladder in the menu is decorative.
	//
	// IT FORCES VSYNC OFF AND THE FRAME CAP TO UNLIMITED before starting, and that is not a
	// convenience: ApplyVideoSettings re-applies t.MaxFPS from the saved settings on EVERY arm, so
	// a player ini carrying a 60 fps cap would make all four presets measure 16.67 ms and the run
	// would be the cap, not a result. That is the trap spec v11 §4 is written about. Both are
	// restored when the run ends. Still measure at r.ScreenPercentage 300: macOS present pacing
	// survives both of these settings.
	//
	//   ... -TraceExec="r.ScreenPercentage 300|Trace.Video.PresetAB 4 1.5 3" -TraceExecAt=14

	/** One arm of the preset A/B. */
	struct FArenaPresetArmSpec
	{
		const TCHAR* Name;
		ETraceVideoQuality Quality;
		bool bBaseline;
	};

	const FArenaPresetArmSpec GArenaPresetArms[] =
	{
		// EPIC IS THE BASELINE, so every delta reads as a saving rather than a cost - this table's
		// job is to tell a struggling player what LOW buys them.
		{ TEXT("epic"),    ETraceVideoQuality::Epic,   true  },
		{ TEXT("high"),    ETraceVideoQuality::High,   false },
		{ TEXT("medium"),  ETraceVideoQuality::Medium, false },
		{ TEXT("low"),     ETraceVideoQuality::Low,    false },
		// The null arm. Byte for byte the same preset as 'epic', sampled at the far end of the
		// cycle from it, so |control - epic| is the noise floor under which nothing may be claimed.
		// Without one, a 3% separation between two presets looks like a result. See the long note
		// on the fidelity table's own control arm.
		{ TEXT("control"), ETraceVideoQuality::Epic,   false },
	};

	FAutoConsoleCommand CmdArenaPresetAB(
		TEXT("Trace.Video.PresetAB"),
		TEXT("Trace.Video.PresetAB [SampleSeconds] [WarmupSeconds] [Cycles] - interleave the LOW, "
		     "MEDIUM, HIGH and EPIC presets exactly as the VIDEO page applies them, plus a null arm, "
		     "and print the frame time of each. Run `r.ScreenPercentage 300` first so the GPU is the "
		     "bottleneck. Forces vsync off and the frame cap to unlimited for the duration."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			UTraceGameUserSettings* const Video = UTraceGameUserSettings::Get();
			if (Video == nullptr)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[ARENAPERF] Trace.Video.PresetAB needs UTraceGameUserSettings and there isn't one. ")
					TEXT("Check Config/DefaultEngine.ini's GameUserSettingsClassName."));
				return;
			}

			// The fidelity overrides would pin the six arena features at one tier across all four
			// arms, which is the one thing that would make this table lie.
			SetArenaFidelityArm(-1, -1, -1, -1, -1, -1);

			// See the header note: leaving these as the player has them can cap every arm at the
			// same number and produce a perfectly consistent, perfectly meaningless table.
			const bool bRestoreVSync = Video->IsVSyncEnabled();
			const int32 RestoreCapIndex = Video->GetFrameRateLimitIndex();
			const ETraceVideoQuality RestoreQuality = Video->GetOverallQuality();
			Video->SetVSyncEnabled(false);
			Video->SetFrameRateLimitByIndex(0);

			FArenaPerfState State;
			State.SampleSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 2.f, 120.f) : 5.f;
			State.WarmupSeconds = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 0.5f, 30.f) : 2.f;
			State.CyclesTotal = (Args.Num() > 2) ? FMath::Clamp(FCString::Atoi(*Args[2]), 1, 20) : 3;
			State.bRebuildBetweenArms = false;

			for (const FArenaPresetArmSpec& Spec : GArenaPresetArms)
			{
				FArenaPerfArm Arm;
				Arm.Name = Spec.Name;
				Arm.Instancing = GArenaInstancing;
				Arm.bIsBaseline = Spec.bBaseline;
				Arm.ApplyArm = [Spec]()
				{
					if (UTraceGameUserSettings* const ArmVideo = UTraceGameUserSettings::Get())
					{
						// EXACTLY what the OVERALL QUALITY row does, and nothing else. Resolution is
						// deliberately excluded: tearing the window down mid-run would invalidate
						// every sample after it, and the preset does not move render scale anyway.
						ArmVideo->SetOverallQuality(Spec.Quality);
						ArmVideo->ApplyVideoSettings(/*bIncludingResolution=*/false);
					}
				};
				State.Arms.Add(MoveTemp(Arm));
			}

			// Put the machine back the way the player left it, including the preset - a measurement
			// harness that silently leaves someone on Low is a bug report waiting to happen.
			State.OnFinished = [bRestoreVSync, RestoreCapIndex, RestoreQuality]()
			{
				if (UTraceGameUserSettings* const EndVideo = UTraceGameUserSettings::Get())
				{
					if (RestoreQuality != ETraceVideoQuality::Custom)
					{
						EndVideo->SetOverallQuality(RestoreQuality);
					}
					EndVideo->SetVSyncEnabled(bRestoreVSync);
					EndVideo->SetFrameRateLimitByIndex(RestoreCapIndex);
					EndVideo->ApplyVideoSettings(/*bIncludingResolution=*/false);
				}
			};

			UE_LOG(LogTraceGame, Display,
				TEXT("[ARENAPERF] preset A/B starting: %d interleaved cycles x %d arms x (%.1fs warmup + ")
				TEXT("%.1fs sample) = %.0fs total. Restoring overall=%s vsync=%d cap=%s afterwards."),
				State.CyclesTotal, State.Arms.Num(), State.WarmupSeconds, State.SampleSeconds,
				State.CyclesTotal * State.Arms.Num() * (State.WarmupSeconds + State.SampleSeconds),
				*UTraceGameUserSettings::DescribeOverallQuality(RestoreQuality),
				bRestoreVSync ? 1 : 0,
				*UTraceGameUserSettings::DescribeFrameRateLimit(
					UTraceGameUserSettings::GetFrameRateLimitOptions().IsValidIndex(RestoreCapIndex)
						? UTraceGameUserSettings::GetFrameRateLimitOptions()[RestoreCapIndex]
						: 0.f));

			StartArenaPerf(State);
		}));

	/**
	 * The readout, because a ladder nobody can see the current rung of is a ladder nobody trusts.
	 *
	 * NOT named Trace.Arena.Fidelity: that is a console VARIABLE, and a command and a variable sharing
	 * a name is fatal at module load in this engine. The project has already been bitten by that once.
	 */
	FAutoConsoleCommand CmdArenaQuality(
		TEXT("Trace.Arena.Quality"),
		TEXT("Trace.Arena.Quality - print the scalability levels, the tier each arena fidelity feature "
		     "resolved to, and whether mesh distance fields (which Lumen needs) are on."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			const Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();
			UE_LOG(LogTraceGame, Display,
				TEXT("[QUALITY] scalability: ViewDistance=%d AA=%d Shadows=%d GI=%d Reflections=%d ")
				TEXT("PostProcess=%d Textures=%d Effects=%d Shading=%d"),
				Levels.ViewDistanceQuality, Levels.AntiAliasingQuality, Levels.ShadowQuality,
				Levels.GlobalIlluminationQuality, Levels.ReflectionQuality, Levels.PostProcessQuality,
				Levels.TextureQuality, Levels.EffectsQuality, Levels.ShadingQuality);

			UE_LOG(LogTraceGame, Display,
				TEXT("[QUALITY] overrides: master=%d GI=%d Reflections=%d AO=%d Shadows=%d Bloom=%d Lamps=%d ")
				TEXT("(-1 = follow scalability). r.GenerateMeshDistanceFields=%d."),
				GArenaFidelity, GArenaFidelityGI, GArenaFidelityReflections, GArenaFidelityAO,
				GArenaFidelityShadows, GArenaFidelityBloom, GArenaFidelityLamps,
				ArenaMeshDistanceFieldsEnabled());

			UWorld* World = FindArenaPerfWorld();
			if (const ATraceArenaBuilder* Builder = ATraceArenaBuilder::Get(World))
			{
				UE_LOG(LogTraceGame, Display, TEXT("[QUALITY] arena: %s"), *Builder->DescribeFidelity());
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[QUALITY] no ATraceArenaBuilder in this world."));
			}
		}));

	// =============================================================================================
	// SPEC v28 §8 — "ALLOW GOALS TO BE SCORED THROUGH EITHER SIDE OF THE GOAL", MEASURED.
	//
	// Trace.Arena.GoalSides drives the REAL scoring path four times per end and reads the SCOREBOARD:
	//
	//   FRONT   a carrier standing in the mouth from the pitch side          -> must score
	//   BACK    a carrier standing in the mouth from the POCKET side         -> must score
	//   WIDE    a carrier the same distance off the plane but 0.75 radii out -> must NOT score
	//
	// THE WIDE ARM IS THE POINT OF THE HARNESS, not decoration. A test that only ever teleports a
	// carrier next to a goal and watches the score go up cannot tell "the goal scores from either
	// side" apart from "anything near the goal scores", and this project has shipped a green harness
	// that measured nothing at least twice.
	//
	// 0.90 radii is chosen to be the DISCRIMINATING case rather than merely a distant one. A carrier
	// standing on a ramp is 952 uu below the mouth centre, so at 900 uu out laterally they are 1310
	// uu from it - outside the 1000 uu disc - while still inside the slab's bounding BOX, which is
	// only +/-1000 in Y. It therefore fails on the ring test specifically. Put it at 1.4 radii and it
	// would fail the box test first and prove nothing about the circle.
	//
	// IT WAS 0.75, AND THAT MEASURED THE ENGINE RATHER THAN THE RULE. At 750 uu out the pawn's origin
	// is 1211 uu from the mouth centre - 7 uu OUTSIDE the ring band's outer radius of 1204, i.e. with
	// its 34 uu capsule buried in the hoop. Teleporting into solid geometry hands the answer to
	// depenetration, which pushed it radially INWARD through the mouth: the arm scored, and reported a
	// failure of a rule that was working. 0.90 clears the band by 106 uu, more than a capsule radius,
	// so the pawn stands in free air on the ramp and the only thing deciding the arm is the disc test.
	//
	// It grants the Core, FREEZES the pawn where it puts it (see SetGoalSideRunnerFrozen - a bot
	// holding the Core steers at the goal, and it will happily walk the red arm into the hoop) and
	// teleports it; everything after that is untouched production code -
	// ATraceCore's swept carry test, ATraceEndzone's 10 Hz poll and its overlap, ATraceGameMode's
	// possession-change test, NotifyScored's debounce. The measurement is the scoreboard delta across
	// the arm, so an arm only passes if a point was actually AWARDED.
	//
	// The pawn is staged on the SAME SIDE of the ring plane it will score from, and given the Core
	// only once it is there, so no arm can be scored by the teleport itself sweeping through the
	// hoop. That is not paranoia: the carry test is a swept segment test, and it would have counted
	// the trip from midfield to the pocket as a goal through the front of the ring.
	// =============================================================================================

	struct FTraceGoalSideArm
	{
		float EndSign = 1.f;        // which end's goal
		float FaceSign = 1.f;       // which side of the ring plane the carrier stands on, in world X
		float LateralRadii = 0.f;   // Y offset from the mouth centre, in mouth radii
		bool bExpectScore = true;
		const TCHAR* Label = TEXT("");
	};

	struct FTraceGoalSideState
	{
		TArray<FTraceGoalSideArm> Arms;
		int32 ArmIndex = 0;
		int32 Phase = 0;
		double PhaseSeconds = 0.0;
		double WaitSeconds = 0.0;

		ETraceTeam Attacker = ETraceTeam::None;
		int32 ScoreBefore = 0;
		TWeakObjectPtr<ATraceCharacter> Runner;

		int32 Passed = 0;
		int32 Failed = 0;
		int32 Skipped = 0;

		/**
		 * How many times this arm has re-granted the Core after the field reset took it back.
		 *
		 * MEASURED: the arm before this one scores, and a score is a whole field reset - kickoff delay,
		 * ten pawns teleported, the Core released and re-granted to the receiving team. The settle
		 * below is generous but the kickoff can still land between this arm's grant and its check, and
		 * the first run of this harness reported "the runner is not the carrier ... SKIPPED" for
		 * exactly that. Re-granting is correct rather than a workaround: what the arm is measuring
		 * starts when the carrier is placed, not when the Core changes hands.
		 */
		int32 Regrants = 0;
	};

	/** How many times an arm may re-take the Core from a post-goal kickoff before giving up. */
	constexpr int32 GoalSideMaxRegrants = 8;

	/** A live pawn on @p Team, preferring one that is not already carrying, or null. */
	ATraceCharacter* FindGoalSideRunner(UWorld* World, ETraceTeam Team)
	{
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Character = *It;
			if (IsValid(Character) && Character->IsAlive() && Character->GetTeam() == Team)
			{
				return Character;
			}
		}
		return nullptr;
	}

	/** World-space point for one arm: on the ring plane's @p FaceSign side, at standing height. */
	FVector GoalSidePoint(const ATraceArenaBuilder& Arena, const FTraceGoalSideArm& Arm, float AxisDistance)
	{
		const FTransform& Frame = Arena.GetActorTransform();
		const FVector Axis = Frame.GetUnitAxis(EAxis::X);
		const FVector Side = Frame.GetUnitAxis(EAxis::Y);
		const FVector Up = Frame.GetUnitAxis(EAxis::Z);

		// The mouth centre, then back along the field axis to the requested distance, then out
		// laterally, then DOWN to where a pawn's origin actually is at the top of the ramp: the ramp
		// top plus a capsule half height. That is the geometry the shipped carry-in relies on (952 uu
		// below the mouth centre, inside a 1000 uu mouth), so the arm measures the real case and not
		// a pawn floating in the middle of the hoop.
		const FVector Centre = Arena.GetGoalRingCentre(Arm.EndSign);
		const float StandingZ = Arena.GoalRampTopZ() + Arena.PlayerHeightUU() * 0.5f;
		const float DropToStanding = Arena.GoalRingCentreZ() - StandingZ;

		return Centre
			+ Axis * (Arm.FaceSign * AxisDistance)
			+ Side * (Arm.LateralRadii * Arena.GoalRingRadius())
			- Up * DropToStanding;
	}

	/**
	 * Freezes (or releases) the arm's runner where it stands.
	 *
	 * WHY AN ARM HAS TO DO THIS, and it is the second thing this harness measured about itself rather
	 * than about the goal. The runner is usually a BOT, and a bot holding the Core steers at the goal
	 * it is attacking - which is the goal the arm has just parked it 146 uu from. The WIDE arm, whose
	 * whole job is to stand somewhere that must NOT score, was therefore being walked into the hoop by
	 * its own AI inside the measurement window and reported a FAIL against a rule that was working.
	 *
	 * MOVE_None holds the pawn exactly where it was put - no falling, no steering, no sliding on the
	 * ramp - so what the arm measures is the geometry at a known point. Released again at the end of
	 * the arm so the bot goes back to playing.
	 */
	void SetGoalSideRunnerFrozen(ATraceCharacter* Runner, bool bFrozen)
	{
		if (!IsValid(Runner))
		{
			return;
		}

		if (UCharacterMovementComponent* Movement = Runner->GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
			Movement->SetMovementMode(bFrozen ? MOVE_None : MOVE_Falling);
		}

		if (AController* Controller = Runner->GetController())
		{
			Controller->StopMovement();
		}
	}

	void TickGoalSideTest(const TSharedRef<FTraceGoalSideState>& State, float DeltaTime);

	void StartGoalSideTest(const TSharedRef<FTraceGoalSideState>& State)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float DeltaTime) -> bool
			{
				TickGoalSideTest(State, DeltaTime);
				return State->ArmIndex < State->Arms.Num();
			}), 0.f);
	}

	void TickGoalSideTest(const TSharedRef<FTraceGoalSideState>& State, float DeltaTime)
	{
		State->PhaseSeconds += DeltaTime;
		if (State->PhaseSeconds < State->WaitSeconds)
		{
			return;
		}

		UWorld* World = FindArenaPerfWorld();
		ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World);
		ATraceCore* Core = ATraceCore::Get(World);
		ATraceGameState* GameState = (World != nullptr) ? World->GetGameState<ATraceGameState>() : nullptr;

		if (World == nullptr || Arena == nullptr || Core == nullptr || GameState == nullptr
			|| World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[GOALSIDES] no arena / Core / game state in this world, or this is a client. ")
				TEXT("Scoring is a server decision; nothing was measured."));
			State->ArmIndex = State->Arms.Num();
			return;
		}

		if (!State->Arms.IsValidIndex(State->ArmIndex))
		{
			return;
		}

		const FTraceGoalSideArm& Arm = State->Arms[State->ArmIndex];

		auto NextArm = [&State](double Settle)
		{
			++State->ArmIndex;
			State->Phase = 0;
			State->PhaseSeconds = 0.0;
			State->WaitSeconds = Settle;
			State->Runner = nullptr;

			if (State->ArmIndex >= State->Arms.Num())
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[GOALSIDES] DONE: %d passed, %d FAILED, %d skipped. A pass means the SCOREBOARD ")
					TEXT("moved (or, on the WIDE arm, did not)."),
					State->Passed, State->Failed, State->Skipped);
			}
		};

		switch (State->Phase)
		{
		case 0:
		{
			// The scoring rule is only armed in a match that is actually running - warm-up and the
			// half-time interval reset the field without counting (ATraceGameMode::NotifyScored). A
			// harness that ran anyway would report every arm as a failure and the failure would be
			// about the clock, not about the geometry. Wait for it, then say what we are measuring.
			if (GameState->TraceMatchState != ETraceMatchState::InProgress || GameState->IsHalfTimeBreak())
			{
				State->PhaseSeconds = 0.0;
				State->WaitSeconds = 1.0;
				UE_LOG(LogTraceGame, Verbose, TEXT("[GOALSIDES] waiting for the match to be in progress."));
				return;
			}

			// The team that ATTACKS this end is the opponent of the team defending it.
			const ETraceTeam Defender = GameState->GetTeamDefendingEnd(Arm.EndSign);
			State->Attacker = TraceOpposingTeam(Defender);

			ATraceCharacter* Runner = FindGoalSideRunner(World, State->Attacker);
			if (Runner == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[GOALSIDES] %s: no live %s pawn to carry it. SKIPPED."),
					Arm.Label, *TraceTeamName(State->Attacker).ToString());
				++State->Skipped;
				NextArm(0.5);
				return;
			}

			// STAGED ON THE SAME SIDE it will score from, and 1200 uu clear of the plane so the
			// staging move itself cannot cross the mouth. See the block comment.
			State->Runner = Runner;
			State->Regrants = 0;
			Runner->SetActorLocation(GoalSidePoint(*Arena, Arm, 1200.f), /*bSweep=*/false,
				nullptr, ETeleportType::TeleportPhysics);
			SetGoalSideRunnerFrozen(Runner, /*bFrozen=*/true);

			State->Phase = 1;
			State->PhaseSeconds = 0.0;
			State->WaitSeconds = 0.35;
			return;
		}

		case 1:
		{
			ATraceCharacter* Runner = State->Runner.Get();
			if (!IsValid(Runner) || !Runner->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[GOALSIDES] %s: the runner died while staging. SKIPPED."), Arm.Label);
				++State->Skipped;
				NextArm(0.5);
				return;
			}

			// The Core is granted HERE, with the pawn already parked on its own side of the plane, so
			// the carry test's first sample is taken from a legal place.
			Core->GrantTo(Runner, ETraceCoreGrantReason::Debug);
			State->ScoreBefore = GameState->GetScore(State->Attacker);

			State->Phase = 2;
			State->PhaseSeconds = 0.0;
			State->WaitSeconds = 0.35;
			return;
		}

		case 2:
		{
			ATraceCharacter* Runner = State->Runner.Get();
			if (!IsValid(Runner) || !Runner->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[GOALSIDES] %s: the runner is gone. SKIPPED."), Arm.Label);
				++State->Skipped;
				NextArm(0.5);
				return;
			}

			// The previous arm's goal reset the field and the kickoff may have taken the Core back
			// between the grant and here. Take it again and re-stage rather than skipping - see
			// FTraceGoalSideState::Regrants.
			if (Core->GetCarrier() != Runner)
			{
				if (State->Regrants >= GoalSideMaxRegrants)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[GOALSIDES] %s: could not keep the Core after %d attempts. SKIPPED."),
						Arm.Label, State->Regrants);
					++State->Skipped;
					NextArm(0.5);
					return;
				}

				++State->Regrants;
				Runner->SetActorLocation(GoalSidePoint(*Arena, Arm, 1200.f), /*bSweep=*/false,
					nullptr, ETeleportType::TeleportPhysics);
				SetGoalSideRunnerFrozen(Runner, /*bFrozen=*/true);
				Core->GrantTo(Runner, ETraceCoreGrantReason::Debug);
				State->ScoreBefore = GameState->GetScore(State->Attacker);
				State->PhaseSeconds = 0.0;
				State->WaitSeconds = 0.6;
				return;
			}

			// Into the mouth (or, on the WIDE arm, alongside it). The distance off the plane is what a
			// carrier at the top of a ramp actually gets to: the hoop's own half thickness plus a
			// capsule radius plus a little.
			const float StandOff = Arena->GoalRingHalfThickness() + TraceArenaConstants::PawnCapsuleRadius + 12.f;
			const FVector Target = GoalSidePoint(*Arena, Arm, StandOff);
			Runner->SetActorLocation(Target, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
			SetGoalSideRunnerFrozen(Runner, /*bFrozen=*/true);

			UE_LOG(LogTraceGame, Display,
				TEXT("[GOALSIDES] %s: %s carrying, placed at %s (%.0f uu off the ring plane on the %s side, ")
				TEXT("%.2f radii out). Score before: %d."),
				Arm.Label, *TraceTeamName(State->Attacker).ToString(), *Target.ToCompactString(),
				StandOff, (Arm.FaceSign * Arm.EndSign > 0.f) ? TEXT("POCKET") : TEXT("PITCH"),
				Arm.LateralRadii, State->ScoreBefore);

			State->Phase = 3;
			State->PhaseSeconds = 0.0;
			State->WaitSeconds = 0.9;
			return;
		}

		default:
		{
			SetGoalSideRunnerFrozen(State->Runner.Get(), /*bFrozen=*/false);

			const int32 ScoreNow = GameState->GetScore(State->Attacker);
			const bool bScored = (ScoreNow > State->ScoreBefore);
			const bool bPass = (bScored == Arm.bExpectScore);

			if (bPass)
			{
				++State->Passed;
			}
			else
			{
				++State->Failed;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[GOALSIDES] %s: %s. Expected a goal: %s. Got one: %s. %s %d -> %d. (Blue %d - Orange %d)"),
				Arm.Label, bPass ? TEXT("PASS") : TEXT("*** FAIL ***"),
				Arm.bExpectScore ? TEXT("yes") : TEXT("no"),
				bScored ? TEXT("yes") : TEXT("no"),
				*TraceTeamName(State->Attacker).ToString(), State->ScoreBefore, ScoreNow,
				GameState->BlueScore, GameState->OrangeScore);

			// Long enough for NotifyScored's whole reset - kickoff delay included - to finish before
			// the next arm stages a pawn, or the next arm would be teleporting a pawn the reset is
			// still moving.
			NextArm(bScored ? 6.0 : 1.0);
			return;
		}
		}
	}

	FAutoConsoleCommand CmdArenaGoalSides(
		TEXT("Trace.Arena.GoalSides"),
		TEXT("Trace (spec v28 section 8): carry the Core into each goal from the PITCH side and from the "
		     "POCKET side behind it, plus a deliberately off-target arm that must NOT score, and report "
		     "the scoreboard delta for each. Server, mode B, match in progress."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			TSharedRef<FTraceGoalSideState> State = MakeShared<FTraceGoalSideState>();

			// Both ends, both faces, and one off-target arm per end. The face sign is expressed
			// against the END so that "pocket" and "pitch" mean the same thing at either end: the
			// pocket is always further from the centre line.
			for (const float EndSign : { -1.f, 1.f })
			{
				const TCHAR* EndName = (EndSign < 0.f) ? TEXT("-X end") : TEXT("+X end");

				State->Arms.Add({ EndSign, -EndSign, 0.f, true,
					(EndSign < 0.f) ? TEXT("-X goal, from the PITCH") : TEXT("+X goal, from the PITCH") });
				State->Arms.Add({ EndSign, EndSign, 0.f, true,
					(EndSign < 0.f) ? TEXT("-X goal, from BEHIND (the pocket)") : TEXT("+X goal, from BEHIND (the pocket)") });
				State->Arms.Add({ EndSign, EndSign, 0.90f, false,
					(EndSign < 0.f) ? TEXT("-X goal, from BEHIND but WIDE (red arm)") : TEXT("+X goal, from BEHIND but WIDE (red arm)") });

				UE_LOG(LogTraceGame, Verbose, TEXT("[GOALSIDES] queued three arms at the %s."), EndName);
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[GOALSIDES] starting: %d arms. Each one grants the Core to a live attacker, parks it "
				     "on one side of the hoop and reads the scoreboard."), State->Arms.Num());

			StartGoalSideTest(State);
		}));

}

// =================================================================================================
// SPEC v28 INTEGRATION — THE GUN'S REACH IS DERIVED FROM THE ARENA'S SIZE, AND NOTHING CHECKED IT.
//
// UTraceSettings::HitscanRange is not an independent knob. It has to span the arena's diagonal or a
// shot down the long axis expires in mid-air short of a target the player can plainly see. It has
// been left behind TWICE now:
//
//   spec v4 §3   lengthened the field 24000 -> 33600 and left the range at 28000 (6944 uu short);
//   spec v28 §8  lengthened it 33600 -> 38400 for the hockey pockets and left it at 36000
//                (3581 uu short). The §8 owner found this and could not fix it - TraceSettings.h is
//                not their file - and said so in their hand-off. This pass raised it to 39600.
//
// A NUMBER THAT MUST TRACK ANOTHER NUMBER NEEDS A HARNESS, NOT A COMMENT. Both comments were there
// and both were missed, which is the whole argument for this command existing.
//
// It measures two different things and reports both:
//
//   THE ARITHMETIC  sqrt(FieldLength^2 + FieldWidth^2) read off the LIVE ATraceArenaBuilder in the
//                   world, against the LIVE UTraceSettings::HitscanRange (which is the .ini's value,
//                   not the header's - the ini wins and that is the layer that has been wrong).
//
//   THE REAL TRACE  a genuine world line trace from just inside one back pocket at the far diagonal
//                   corner, run at exactly HitscanRange. A pass means it reached blocking geometry;
//                   a fail means it died in the air. This is what the PLAYER experiences, and it is
//                   the half that cannot be fooled by getting the arithmetic right against a field
//                   size the builder does not actually use.
//
// RED ARM: Trace.Arena.HitscanReachArm <uu> runs the whole thing again at a forced range, so
// `Trace.Arena.HitscanReachArm 36000` reproduces the shipped-before-this-pass defect in this same
// binary and must FAIL. Set it to 0 to go back to the real setting.
// =================================================================================================
namespace
{
	float GTraceHitscanReachArmUU = 0.f;

	FAutoConsoleVariableRef CVarTraceHitscanReachArm(
		TEXT("Trace.Arena.HitscanReachArm"),
		GTraceHitscanReachArmUU,
		TEXT("Trace: force Trace.Arena.VerifyHitscanReach to measure a given range in uu instead of the "
		     "shipped UTraceSettings::HitscanRange. 36000 reproduces the pre-v28-integration defect. 0 = off."),
		ECVF_Default);

	ATraceArenaBuilder* FindHitscanReachArena(UWorld*& OutWorld)
	{
		OutWorld = nullptr;
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE))
			{
				continue;
			}

			for (TActorIterator<ATraceArenaBuilder> It(World); It; ++It)
			{
				if (IsValid(*It))
				{
					OutWorld = World;
					return *It;
				}
			}
		}

		return nullptr;
	}

	FAutoConsoleCommand CmdTraceVerifyHitscanReach(
		TEXT("Trace.Arena.VerifyHitscanReach"),
		TEXT("Trace: assert UTraceSettings::HitscanRange spans the LIVE arena's diagonal, and fire a real "
		     "world trace down that diagonal to prove it reaches. Red arm: Trace.Arena.HitscanReachArm 36000."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			UWorld* World = nullptr;
			ATraceArenaBuilder* Arena = FindHitscanReachArena(World);

			if (Arena == nullptr || World == nullptr)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[HITSCANREACH] no ATraceArenaBuilder in any game world - nothing to measure. Run this "
					     "on a loaded arena map (/Game/Maps/Arena_Baked or the procedural one)."));
				return;
			}

			const float ArmUU     = GTraceHitscanReachArmUU;
			const bool  bArmed    = (ArmUU > 0.f);
			const float ShippedUU = FMath::Max(1.f, UTraceSettings::Get().HitscanRange);
			const float RangeUU   = bArmed ? ArmUU : ShippedUU;

			// The requirement is the WALL-TO-WALL diagonal, not the goal-to-goal one. A player standing
			// in one back pocket can see - and must be able to shoot - a player in the far one.
			const float LengthUU   = Arena->FieldLength;
			const float WidthUU    = Arena->FieldWidth;
			const float DiagonalUU = FMath::Sqrt(LengthUU * LengthUU + WidthUU * WidthUU);

			int32 Failures = 0;

			UE_LOG(LogTraceGame, Display,
				TEXT("================================================================================"));
			UE_LOG(LogTraceGame, Display,
				TEXT("[HITSCANREACH] the gun's reach against the arena it has to cross.%s"),
				bArmed ? TEXT("  *** RED ARM: Trace.Arena.HitscanReachArm is ON ***") : TEXT(""));
			UE_LOG(LogTraceGame, Display,
				TEXT("[HITSCANREACH] arena %.0f x %.0f uu (read off the live builder) -> diagonal %.0f uu."),
				LengthUU, WidthUU, DiagonalUU);
			UE_LOG(LogTraceGame, Display,
				TEXT("[HITSCANREACH] HitscanRange in force %.0f uu%s."),
				RangeUU,
				bArmed ? *FString::Printf(TEXT(" (FORCED by the red arm; the shipped setting is %.0f)"), ShippedUU)
				       : TEXT(" (UTraceSettings, i.e. Config/DefaultGame.ini layered over the header)"));

			// ---- 1. the arithmetic ----------------------------------------------------------------
			const bool bSpansDiagonal = (RangeUU >= DiagonalUU);
			Failures += bSpansDiagonal ? 0 : 1;

			if (bSpansDiagonal)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[HITSCANREACH] OK    the range SPANS the diagonal with %.0f uu of margin."),
					RangeUU - DiagonalUU);
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[HITSCANREACH] FAIL  the range is %.0f uu SHORT of the diagonal. A shot down the long "
					     "axis expires in mid-air short of a target on screen. Raise HitscanRange in BOTH "
					     "Source/Trace/TraceSettings.h AND Config/DefaultGame.ini (the ini wins) to at least %.0f."),
					DiagonalUU - RangeUU, FMath::CeilToFloat(DiagonalUU / 100.f) * 100.f);
			}

			// ---- 2. the real trace -----------------------------------------------------------------
			// WHAT THIS ARM MEASURES, precisely, because two earlier versions of it did NOT measure what
			// they printed and both are worth recording:
			//
			//   v1  LineTraceSingle down the diagonal. It stops at the FIRST blocker, which on a real
			//       arena is a piece of cover ~7000 uu away, and the code then announced that the
			//       39098 uu sight line was shootable. That does not follow at all.
			//   v2  LineTraceMULTI, taking the furthest hit. Same number. A multi trace collects TOUCH
			//       hits along the way and then STOPS at the first BLOCKING hit - it does not continue
			//       past solid geometry - so "the furthest hit" was still the first blocker.
			//
			// A harness that reports something it did not measure is worse than no harness, and this
			// project has shipped one before, so the fix is a real walk down the ray: trace, ignore the
			// actor that stopped it, trace again from the SAME origin along the SAME direction. Each
			// pass reveals the next piece of geometry further out; the LAST one found is the far end
			// wall, because there is nothing behind it. That distance is the true length of the sight
			// line from one back pocket to the opposite one, measured on the baked collision rather
			// than computed from two numbers in a header.
			//
			// The probe is deliberately run LONGER than the range under test, so that the measurement
			// of the ARENA does not depend on the SETTING being measured; the assertion is then made
			// against the range.
			const float InsetUU  = 200.f;
			const float EyeZUU   = 160.f;
			const float HalfLen  = LengthUU * 0.5f - InsetUU;
			const float HalfWide = WidthUU  * 0.5f - InsetUU;

			const FVector ArenaOrigin = Arena->GetActorLocation();
			const FVector Start = ArenaOrigin + FVector(-HalfLen, -HalfWide, EyeZUU);
			const FVector Far   = ArenaOrigin + FVector(+HalfLen, +HalfWide, EyeZUU);
			const FVector Dir   = (Far - Start).GetSafeNormal();
			const float   ProbeUU = static_cast<float>(FVector::Dist(Start, Far)) + 4000.f;

			FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceHitscanReach), /*bTraceComplex=*/false);
			Params.bReturnPhysicalMaterial = false;

			// 256 is a ceiling, not an expectation: the arena is ~1295 instanced blocks in 64 pools and
			// only the handful actually on this one ray can be hit. If the walk ever runs out of steps
			// the result is reported as INCOMPLETE rather than as a measurement, below.
			const int32 MaxSteps = 256;

			float   SightLineUU  = 0.f;
			FString FurthestName = TEXT("nothing");
			int32   Blockers     = 0;
			int32   Steps        = 0;

			for (; Steps < MaxSteps; ++Steps)
			{
				FHitResult Hit;
				if (!World->LineTraceSingleByChannel(Hit, Start, Start + Dir * ProbeUU, ECC_Visibility, Params))
				{
					break;   // nothing left along this ray - the previous hit was the far wall
				}

				++Blockers;
				if (Hit.Distance > SightLineUU)
				{
					SightLineUU  = Hit.Distance;
					FurthestName = GetNameSafe(Hit.GetActor());
				}

				AActor* Blocker = Hit.GetActor();
				if (Blocker == nullptr)
				{
					break;   // a hit with no actor cannot be ignored, so the walk cannot continue
				}
				Params.AddIgnoredActor(Blocker);
			}

			const bool bWalkComplete = (Steps < MaxSteps);

			if (Blockers == 0)
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[HITSCANREACH] FAIL  a %.0f uu probe down the corner-to-corner line hit NOTHING AT ALL, not "
					     "even the far wall. That is not a range problem - the arena's collision is missing, or this "
					     "is not an arena map - and nothing below can be trusted."),
					ProbeUU);
			}
			else if (!bWalkComplete)
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[HITSCANREACH] FAIL  the ray walk ran out of steps after %d blockers without reaching open "
					     "space, so %.0f uu is a LOWER BOUND on the sight line and not a measurement. Raise MaxSteps."),
					Blockers, SightLineUU);
			}
			else
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[HITSCANREACH] walked the ray in %d step(s): %d blocker(s) between the two back pockets, and "
					     "the FURTHEST - the far end wall, with nothing behind it - is %s at %.0f uu. That distance IS "
					     "the corner-to-corner sight line, measured on the baked collision."),
					Steps + 1, Blockers, *FurthestName, SightLineUU);

				const bool bReaches = (RangeUU >= SightLineUU);
				Failures += bReaches ? 0 : 1;

				if (bReaches)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[HITSCANREACH] OK    a %.0f uu shot COVERS that %.0f uu sight line with %.0f uu to spare. "
						     "A player standing in one back pocket can shoot one standing in the other."),
						RangeUU, SightLineUU, RangeUU - SightLineUU);
				}
				else
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("[HITSCANREACH] FAIL  a %.0f uu shot dies %.0f uu SHORT of that %.0f uu sight line. This is "
						     "what the player sees: a target on screen that cannot be hit, with nothing saying why."),
						RangeUU, SightLineUU - RangeUU, SightLineUU);
				}
			}

			// The 3D box diagonal is REPORTED and deliberately NOT asserted. sqrt(L^2 + W^2 + H^2)
			// includes the roof, and it is not a distance two PLAYERS can be apart: they stand on the
			// floor and on 352 uu banks, so the elevation difference between a shooter and a target is
			// a few hundred uu, not the wall's full height. Asserting it would demand ~39670 uu of
			// reach to cover a shot from a corner of the floor to the opposite corner of the CEILING,
			// which nothing in this game can take.
			const float BoxDiagonalUU = FMath::Sqrt(LengthUU * LengthUU + WidthUU * WidthUU
				+ Arena->WallHeight * Arena->WallHeight);
			UE_LOG(LogTraceGame, Display,
				TEXT("[HITSCANREACH] for reference, the full 3D box diagonal (floor corner to opposite ROOF corner) "
				     "is %.0f uu. Not asserted: two players cannot be that far apart - the roof is %.0f uu up and "
				     "they are on the floor and on %.0f uu banks."),
				BoxDiagonalUU, Arena->WallHeight, 352.f);

			// ---- verdict ----------------------------------------------------------------------------
			if (Failures == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[HITSCANREACH] VERDICT: PASS - 2 checks, 0 failed. The gun reaches across the whole arena.%s"),
					bArmed ? TEXT("  (RED ARM PASSED - the forced range is long enough, so this arm proves nothing.)")
					       : TEXT(""));
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[HITSCANREACH] VERDICT: FAIL - %d of 2 checks failed.%s"),
					Failures,
					bArmed ? TEXT("  This is the RED ARM and a failure here is the point: it is the defect reproduced.")
					       : TEXT(""));
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("================================================================================"));
		}));
}
