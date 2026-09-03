// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"      // EComponentMobility, EEndPlayReason
#include "GameFramework/Actor.h"
#include "Math/Box.h"                // FBox
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"              // ETraceTeam
#include "Core/TraceMatchTypes.h"    // TraceIsGoalMode / TraceScoringModeLabel, and ETraceScoringMode
                                     // itself via TraceSettings.h - the A/B toggle (spec v4 §7)
#include "World/TraceBakedPiece.h"   // ETraceBakedScoringTag - the bake's mode tag (spec v15 §1)

#include "TraceArenaBuilder.generated.h"

class ADirectionalLight;
class APostProcessVolume;
class ASkyLight;
class ATraceCoreSpawn;
class ATraceEndzone;
class ATraceTeamPlayerStart;
class UBoxComponent;
class UDirectionalLightComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPointLightComponent;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Builds the entire playfield from C++ at BeginPlay, so /Game/Maps/Arena can be a completely
 * empty level and the project never ships an authored level .uasset (build contract section 2).
 *
 * THAT IS WHY THE MAP LOOKS EMPTY IN THE EDITOR. Drop one of these into the level and press
 * "Build Preview In Editor" in its Details panel to see the arena without pressing Play - the full
 * two-click workflow, and the reasons it cannot leak into a build, are documented on that function.
 *
 * WHAT IT MAKES  (spec v3 section 7 - rebuilt from the collaborator's overhead sketch,
 *                 lengthened to 3.5 : 1 for spec v4 section 3)
 * -------------
 * A 38400 x 9600 uu Tron arena - 33600 of it goal to goal, plus a 2400 uu HOCKEY POCKET behind each
 * goal (spec v28 §8). Inside it:
 *
 *  - a near-black glossy floor carrying a team-tinted neon grid, and four 2600 uu perimeter walls
 *    with lit trim, a kick rail and vertical ribs;
 *  - a SHALLOW STADIUM BOWL: four terraced corner banks, one per quadrant, raised along the long
 *    edges and stepping DOWN toward the flat central playfield. These are the sketch's green
 *    arrows. Every riser is under MaxStepHeight so they are walkable from any direction and cannot
 *    trap a bot that steers straight at its target (see BuildCornerBanks);
 *  - a SMALL DIAMOND at the exact centre - a three-tier stepped platform carrying the Core
 *    pedestal - ringed by four light pylons;
 *  - a TALL 3.5x-player-height tower at top centre, standing on the dividing line;
 *  - a scatter of cover boxes and long low bars at exactly 1x / 2x / 3.5x player height (176 / 352
 *    / 616 uu), keyed to the character capsule via PlayerHeightUU();
 *  - a lit gate spanning the full width of the field just in FRONT of each goal line (it used to
 *    stand on the line; the hoop hangs there now - see BuildEndzones); the endzones themselves run
 *    sideline to sideline - see EndzoneHalfWidth;
 *  - and, for MODE B, a GOAL at each end: since spec v6 section 4.3 that is a 2000 uu CIRCULAR hoop
 *    (GoalHalfWidth is its radius) with its bottom raised 1.5 player heights off the floor, and
 *    since spec v28 section 8 it FLOATS on the goal line rather than sitting in the back wall, with
 *    a run-up ramp on each face and 2400 uu of playable pocket behind it. See BuildGoalRing.
 *
 * TWO SCORING SHAPES, BOTH BUILT (spec v4 section 7)
 * -------------------------------------------------
 * Mode A scores in full-width endzones; mode B scores in narrow, finite-height goals. The arena
 * builds BOTH sets of furniture - both pairs of ATraceEndzone volumes and both pieces of paint - and
 * then ARMS one of them (ApplyScoringMode). That is what makes the A/B toggle a flag flip a match
 * can survive rather than a rebuild that needs a restart, which is the whole point of an A/B test.
 *
 * The cost is about 80 extra components and two extra server-only trigger actors, all of which are
 * hidden and collisionless in the mode that is not being played. The alternative - rebuilding the
 * endzone furniture on the toggle - was rejected because the toggle can be flipped while ten pawns
 * are standing on the geometry being destroyed.
 *
 * NOTHING HERE DECIDES WHICH MODE IS RUNNING. The builder is told, through ApplyScoringMode /
 * ApplyScoringModeInWorld, by the one authority there is: ATraceGameState::GetScoringMode(). See the
 * note at the top of TraceMatchTypes.h.
 *
 * Along both flanks: a row of wall buttresses carrying a continuous high rail, a light bridge per
 * lane pylon out to the side wall, bright lane floor stripes and a pylon in each endzone corner.
 * Plus the gameplay furniture: FOUR ATraceEndzone triggers (two endzones and two goals - see the
 * two-modes note above), five ATraceTeamPlayerStarts per team, the lighting rig (three directional
 * lights plus a 32-lamp floor lattice), height fog and an unbound post-process volume.
 *
 * HOW MUCH OF THAT REACHES THE RENDERER (spec v7 §8, and this replaces the old "~1190 components"
 * line, which was true and misleading in equal measure). The build still describes ~1240 pieces of
 * geometry, but they are no longer ~1240 primitives: every visible block is now an INSTANCE inside a
 * pooled UInstancedStaticMeshComponent, batched by mesh + material + shadow flag, so the ~960 loose
 * Movable static-mesh components the arena used to register collapse into a few dozen Static pools.
 * The ~280 collision boxes and pawn shells are unchanged in number but are Static now too, which is
 * what keeps them out of the physics scene's dynamic broadphase. Nothing about the LAYOUT changed:
 * same shapes, same transforms, same materials. See the INSTANCING block above AddInstancedBlock,
 * and measure any claim about it with Trace.Arena.PerfAB rather than by reading this comment.
 *
 * MIRRORED HALVES, DELIBERATELY
 * -----------------------------
 * The hand sketch is not symmetric - the left half has a long horizontal bar and a diagonal, the
 * right a vertical bar and a different scatter. This builder mirrors ONE half through the centre
 * line instead, because the match plays two halves with a side switch (spec section 8) and an
 * asymmetric field would hand one team the better half for ten minutes. Everything except the
 * top-centre tower is mirrored in X; the tower sits ON the dividing line, so it belongs to neither
 * half and mirroring leaves it where it is. If the asymmetry is ever wanted back, it is a per-spec
 * XSign filter in BuildCoverField and nothing else.
 *
 * WHY IT IS THIS BIG
 * ------------------
 * The field was 8000 x 4000. At WalkSpeed 720 that is 11 seconds end to end and a point was over
 * before it started. 24000 long was 3x that; 33600 is 4.2x, and at the current WalkSpeed of 800 a
 * full-field run is ~42 SECONDS. That is a long time, and the report on this pass says so plainly:
 * if it plays badly, the alternative reading of "lengthen to 3.5:1" is to NARROW instead, which is
 * two numbers on FieldLength/FieldWidth below (24000 / 6857) and nothing else - every structure,
 * volume, spawn and bound in this file is derived from those two, and the corner banks, the goal and
 * the endzone all re-derive themselves. It is genuinely a one-edit change; it is not a rewrite.
 *
 * Every derived number below is expressed as a fraction of the field (or, for the pieces that must
 * not drift away from the goal line when the field grows, as an offset back FROM the goal line - see
 * ApproachCover in the .cpp), so changing FieldLength/FieldWidth moves the whole layout coherently.
 *
 * ART DIRECTION - READ THIS BEFORE TOUCHING THE LIGHTING
 * -----------------------------------------------------
 * Tron is a DARK world lit by neon emissive edges. Everything here is arranged around that:
 *
 *  - Structural surfaces use M_TraceSurface with albedos in the 0.011-0.07 range. The FLOOR is the
 *    only near-mirror (roughness 0.16) so it reflects the neon back at you; everything else is matte
 *    (~0.5) because a glossy 900 uu cover block catches one enormous soft directional-light specular
 *    smear across its whole face. Raise these albedos much further to "see the geometry better" and
 *    you get the grey box this replaced; lower them and the arena becomes neon outlines floating in
 *    a void, which is what the first pass at this actually did - see the measured note in the
 *    palette.
 *  - Every edge that defines a shape is a separate thin block using M_TraceNeon, an UNLIT opaque
 *    material whose emissive is Color * Glow. Glow is deliberately > 1 so the value clears the
 *    post-process bloom threshold; bloom is what turns a coloured strip into a glowing tube.
 *  - The sun is BELOW the horizon (positive pitch on AtmosphereSunRotation) and dim. It exists only
 *    to give ASkyAtmosphere something to scatter, which produces a near-black sky with a faint
 *    horizon gradient, which the real-time sky light then captures as a small amount of cool
 *    ambient. That ambient is the only thing keeping unlit-facing surfaces off pure black, so do
 *    not delete the atmosphere - that is the black-screen bug this project already shipped once.
 *  - The visible shaping comes from KeyLight / FillLight / BounceLight, all deliberately weak.
 *
 * NET MODEL - the important part:
 *  - The GameMode spawns exactly one of these on the server. The actor replicates so that clients
 *    get a copy, but it has no replicated properties: every client runs BeginPlay and builds the
 *    same geometry locally from the same constants. Replicating ~260 static meshes as individual
 *    actors would cost real bandwidth and buy nothing, and clients need the collision locally
 *    anyway or predicted movement would fall through the floor.
 *  - Consequence: the layout constants below must be identical on both ends. They are compiled
 *    defaults, so they are - but if you ever expose them to config or a Blueprint subclass, the
 *    server and clients must resolve the same values.
 *  - Server-only pieces: the ATraceEndzone triggers (scoring is a server decision) and the
 *    ATraceTeamPlayerStarts (only ChoosePlayerStart ever reads them).
 *  - Client-and-listen-server-only pieces: lights, fog, post process, every mesh. A dedicated
 *    server builds collision only.
 *
 * ASSET DEPENDENCY  (spec v17 §3)
 * ------------------------------
 * The two parent materials are COMMITTED ASSETS at /Game/Trace/Materials/Parents/, authored by
 * Scripts/generate_content.py. The old note here said they lived in the gitignored
 * Content/Generated/ because "the repo stays text-only" - that policy is retired; the repo has
 * shipped a 639-file baked map since spec v15, and spec v17 §3 requires that nothing the game ships
 * depends on a gitignored asset.
 *
 * Three arms, in order, all in ResolveArenaMaterials(), and the one that answered is LOGGED:
 *   1. /Game/Trace/Materials/Parents/M_*        committed, cooked, normal.
 *   2. /Game/Generated/Materials/M_*            legacy generator output, dev machines only.
 *   3. /Engine/BasicShapes/BasicShapeMaterial   flat and lit. The arena plays; it is not neon.
 */
UCLASS()
class TRACE_API ATraceArenaBuilder : public AActor
{
	GENERATED_BODY()

public:
	ATraceArenaBuilder();

	//~ Contract surface (spec section 7)

	/**
	 * First arena builder in @p World, or null. Cheap enough for occasional use, not per-tick.
	 *
	 * Takes a CONST world and const_casts internally, exactly as ApplyScoringModeInWorld and
	 * ApplyTeamSidesInWorld already did. Finding an actor does not modify the world; the const_cast is
	 * paid for by TActorIterator, which has no const form. Widened here rather than at the call sites
	 * because "I have a const UWorld* and want to ask the arena a question" is the normal case for
	 * every const query in the codebase (ATraceCore's resting-surface test is the one that found it),
	 * and the alternative is a const_cast repeated at each of them.
	 */
	static ATraceArenaBuilder* Get(const UWorld* World);

	/**
	 * World-space point the Core spawns at and resets to.
	 *
	 * A PLACED ATraceCoreSpawn IN THE LEVEL WINS (spec v17 §2, closing spec v15 §1.4). With no marker
	 * this is the derived point it has always been: just above the centre pedestal. Either way it is
	 * the ONE answer — ATraceGameMode::SpawnCoreIfNeeded and ATraceCore::GetHomeLocation() both come
	 * through here, so kickoff, reset-after-score and the "is the Core home" test cannot disagree.
	 */
	FVector GetCoreSpawnLocation() const;

	/**
	 * PATCH 28 §5. ONE SURF RAIL, DESCRIBED, SO NOTHING ELSE HAS TO KNOW WHERE IT IS.
	 *
	 * The movement slice's speed-gain rig (-TraceSurfTest) has to put a pawn on a real ramp, and the
	 * only honest way to do that is to ASK the thing that built it. A rig holding its own copy of the
	 * rail's coordinates would keep passing after the level moved — this project's standing lesson
	 * about two copies of one rule, applied to geometry.
	 */
	struct FTraceSurfRailProbe
	{
		/** False if the rails are switched off, or the field is too small to fit one. */
		bool bValid = false;

		/** A point a few uu OFF the ridable face, high on the arc, near the entry end. */
		FVector FaceEntry = FVector::ZeroVector;

		/** Outward normal of the facet under FaceEntry. Unit length. */
		FVector FaceNormal = FVector::ZeroVector;

		/** Unit vector along the rail, pointing AWAY from the halfway line (the run direction). */
		FVector RunDirection = FVector::ZeroVector;

		/** Length of the ridable run, uu. */
		float RunLength = 0.f;

		/** Crest height above the floor, uu. */
		float Height = 0.f;

		/** The arc's shallowest and steepest facet slopes, in degrees from horizontal. */
		float MinFaceAngleDegrees = 0.f;
		float MaxFaceAngleDegrees = 0.f;

		/**
		 * A point standing on the WALKABLE crest, mid-run. The negative control: a pawn dropped here
		 * must never enter the surf state, because the crest is flat and the floor is flat and neither
		 * is a surf plane.
		 */
		FVector CrestStand = FVector::ZeroVector;

		/**
		 * DEMO 29 ITEM 4. Where the arc's TOE meets the arena FLOOR, a third of the way along the run.
		 *
		 * This is the point a player running along the outer lane actually arrives at, and it exists
		 * because the owner's second complaint ("it still doesn't feel like you can surf INTO curves")
		 * is a claim about the APPROACH, not about a ride already in progress. A rig that can only
		 * start a ride by teleporting a pawn onto the face cannot measure it. Z is the floor, so a
		 * capsule is placed by adding its own half height and nothing else.
		 */
		FVector ToeOnFloor = FVector::ZeroVector;
	};

	/**
	 * Describes the surf rail in the quadrant (@p XSign, @p YSign). Safe to call before or after the
	 * build: it derives everything from the same accessors the build uses, so it cannot disagree with
	 * what was actually constructed.
	 */
	FTraceSurfRailProbe GetSurfRailProbe(float XSign, float YSign) const;

	/**
	 * The SIDE-WALL RIDE described in the same terms, so the surf harness can ride it without a second
	 * copy of anything. -TraceSurfBankTest swaps this in for GetSurfRailProbe() and every arm of
	 * TraceMovementSurf.cpp — the twelve-run ladder, the exit test, the approach test and the two
	 * negative controls — then measures the band instead of a rail. A rig with its own idea of where
	 * the ramp is keeps passing after the level moves; this is the same lesson GetSurfRailProbe's own
	 * header states, applied to the structure that replaced it.
	 *
	 * CrestStand is the BENCH (level ground behind the crest) and ToeOnFloor is the foot of the exit
	 * chain, which is the only place on this ride a player can reach from the floor — those two are
	 * where the negative control and the approach test respectively belong on this geometry, and they
	 * are not the same places they are on a rail.
	 */
	FTraceSurfRailProbe GetSurfBankProbe(float XSign, float YSign) const;

	/**
	 * THE HAND-PLACED SIDE RAMPS, FOUND BY TRACING THEM RATHER THAN BY KNOWING WHERE THEY ARE.
	 *
	 * The two structures above are described from the build's own accessors, which is right for
	 * geometry this class builds. The side ramps are NOT built here: they are `Kit_Ramp_03/04`, two
	 * StaticMeshActors a human placed on /Game/Maps/Arena_Baked, and this class has never heard of
	 * them. An accessor that carried their coordinates would be a second copy of a level's layout
	 * living in C++ — exactly the failure GetSurfRailProbe's own header warns about, and worse,
	 * because the level can be edited without recompiling.
	 *
	 * So this one MEASURES. It marches downward line traces in from the side wall face at one X
	 * station, reads the LIVE ImpactNormal at each sample, and reports the longest contiguous run of
	 * samples whose normal Z is strictly inside (@p MinNormalZ, @p MaxNormalZ) — the band handed in by
	 * the caller, which is the movement component's own live band and not a copy of it. Everything
	 * else follows from that run:
	 *
	 *   FaceEntry     two thirds up the ridable run, 120 uu off along its measured normal
	 *   FaceNormal    the traced normal at that sample, not a derived one
	 *   ToeOnFloor    where the ramp surface last stands within 4 uu of the floor, i.e. its real foot
	 *   CrestStand    the middle of the WALKABLE apron BELOW the band — the negative control belongs
	 *                 on the same structure, because "you cannot surf a walk-up" is the claim
	 *   RunLength     stepped along the run until the band is lost, so it measures the ramp and not
	 *                 the field
	 *
	 * IT CAN FAIL, AND THAT IS THE POINT. bValid is false when no sample in the sweep is inside the
	 * band: on a map whose side wall is bare floor, or on a ramp too shallow to surf, this returns
	 * nothing and the harness reports "this level has no ridable side ramp" instead of teleporting a
	 * pawn into empty air and blaming the movement code.
	 *
	 * @param MinNormalZ the surf band's FLOOR (steep end), from the live movement component.
	 * @param MaxNormalZ the walkable limit, from the live movement component. Samples at or above it
	 *                   are the walk-up, and are where CrestStand goes.
	 */
	FTraceSurfRailProbe GetSideRampProbe(float XSign, float YSign, float MinNormalZ, float MaxNormalZ) const;

	/**
	 * The placed Core spawn marker, or null when the level has none (which is the normal state of
	 * /Game/Maps/Arena). Warns, once per resolve, if there is more than one.
	 */
	ATraceCoreSpawn* FindPlacedCoreSpawn() const;

	/** World-space playable volume: floor to wall top, inside faces of the four walls. */
	FBox GetFieldBounds() const;

	/**
	 * ONE STRAIGHT SWEEP of the ride's cross-section: where it starts, which way it goes, how fast the
	 * whole section sinks, and how far its boxes lap past each end. The ride is a chain of these and
	 * NOTHING ELSE, which is what makes it measurable: Trace.Arena.SurfBankProfile walks the same list
	 * the build did, so the instrument cannot be pointed at a surface the build never made.
	 */
	struct FTraceSurfBankSection
	{
		FVector2D StartTop = FVector2D::ZeroVector;   // plan point of the TOP EDGE at the start
		FVector2D Dir = FVector2D(1.f, 0.f);          // unit plan direction of travel
		FVector2D Out = FVector2D(0.f, -1.f);         // unit plan direction from crest toward toe
		float Length = 0.f;                           // plan length
		float Sink = 0.f;                             // rise/run the whole cross-section sinks at
		float StartZ = 0.f;                           // Z of the top edge at the start
		float StartLap = 0.f;
		float EndLap = 0.f;
	};

	/**
	 * The whole ride along one side wall, in build order: the level run, then an exit chain off each
	 * end. @p PlanSegments is how finely the exits' turn is cut — the build asks for the collision
	 * count on one pass and SurfBankVisualTurnMultiple times it on the other, and the probe asks for
	 * the collision count because that is the surface a player is actually standing on.
	 */
	void GetSurfBankSections(float YSign, int32 PlanSegments, TArray<FTraceSurfBankSection>& OutSections) const;

	/**
	 * The cross-section, in its own frame: how far OUT from the crest and how far DOWN from it the
	 * face is at this face angle. Zero at the crest; the full run and rise at the toe. One definition,
	 * used by the build, by the probe and by SurfBankJointReach.
	 */
	void SurfBankStation(float Degrees, float& OutOutward, float& OutDrop) const;

	// --- THE SIDE-WALL RIDE'S ORACLE, and it is PUBLIC for the reason PlayerHeightUU() is -----------
	//
	// Trace.Arena.SurfBankProfile has to sample finer than the smallest feature of this structure, and
	// it cannot know how fine that is from a second copy of the arithmetic. Every number the rig needs
	// comes from the functions the BUILD used, so an instrument that agrees with a stale constant is
	// not possible here. See BuildSurfBanks.
	/**
	 * Depth of the band's CREST off the side wall face, uu. ButtressDepth + SurfBankTopClearance; the
	 * buttress row is the thing that decides it. Zero when the band is not being built, which is what
	 * the cove reads to know whether to truncate itself early.
	 */
	float SurfBankTopDepth() const;

	/**
	 * Depth of the band's TOE off the side wall face, uu — where the surf face hands over to the
	 * walkable cove underneath it. DERIVED, by bisection, from one requirement:
	 *
	 *     the band's crest lands exactly on SurfBankCrestZ(), which is the arena's 3.5x structure
	 *     height — the same height as the surf rails this replaces.
	 *
	 * The toe is wherever the cove envelope has to stop for the surf arc above it to gain the remaining
	 * height at the band's own slopes, so it moves coherently with BankHeight, BankDepth, the player's
	 * own capsule and the movement component's live surf band. Nothing here is typed.
	 */
	float SurfBankToeDepth() const;

	/** Radius of the circular arc the band's face is cut from, uu. Follows from the toe and the band. */
	float SurfBankFaceRadius() const;

	/**
	 * Thickness of one face slab along its own normal, uu. DERIVED from the crest and the shallow end
	 * of the cut so that a slab always reaches from the ride surface to under the floor — which is why
	 * this structure carries no fill boxes at all. See SurfBankFacetThicknessMargin.
	 */
	float SurfBankFacetThickness() const;

	/** Crest height of the band above the floor, uu — BankHeight, by construction of the toe. */
	float SurfBankCrestZ() const;

	/**
	 * The band's face angles, and they are THE RAILS' READING of the movement component rather than a
	 * second one: the surf band is read off the movement CDO exactly once per process, and two
	 * structures asking separately is how one of them ends up cut to a stale band.
	 */
	float SurfBankMinFaceAngleDegrees() const;
	float SurfBankMaxFaceAngleDegrees() const;


	/**
	 * The worst signed mismatch, in plan length, between a swept section's box (cut square to its own
	 * sweep) and the plane where it meets the level run. The build laps and sinks the exit chain by
	 * this at BOTH signs; the probe uses it as the FEATURE SIZE it must out-sample. One definition.
	 */
	float SurfBankJointReach() const;

	/**
	 * |X| where the level run ends and the exit chain begins, and |X| where the exit chain's far end
	 * lands. The probe needs both to know where to sample finely.
	 */
	void SurfBankRunX(float& OutRunEndX, float& OutExitEndX) const;

	/** Half the field width, uu. The ride is measured in depth off the side wall face. */
	float SurfBankHalfWidth() const;

	/**
	 * ONE PLAYER HEIGHT, in uu, and the single source of every structure height in the arena.
	 *
	 * Spec v3 section 7 keys the sketch's three structure classes to the player: a green outline is
	 * 1x, orange 2x and red 3.5x. Those are 176 / 352 / 616 uu today, but they are only correct
	 * while the capsule is 88 uu half height - so this READS THE CAPSULE rather than hard-coding
	 * 176. ATraceCharacter's class default object is asked for its capsule half height and doubled;
	 * if the CDO cannot be reached (it always can, but the arena must never crash over art) it
	 * falls back to the documented 176 and says so in the log.
	 *
	 * Consequence worth knowing: shrink the capsule and every cover block in the arena shrinks with
	 * it, which is what "1x player height" is supposed to mean.
	 *
	 * PUBLIC since spec v28 §8: it is the arena's answer to "how big is a person", and the goal-side
	 * harness places a pawn at ramp height + half a capsule. A second copy of 88 in a test is a test
	 * that keeps passing after somebody resizes the pawn.
	 */
	float PlayerHeightUU() const;

	/**
	 * EndzoneDepth clamped to something that can actually be built: at least 100 uu, never more than
	 * the half length (a deeper zone than that would swallow the centre of the field).
	 *
	 * THE ONE PLACE THIS CLAMP LIVES. It used to be repeated at three call sites - the trigger, the
	 * builder's own spawn line and ATraceGameMode::BuildEndzoneSpawnPads - and the failure mode of a
	 * clamp that disagrees with itself is not a compile error, it is a spawn pad on the wrong side of
	 * a goal line. Call this instead of clamping EndzoneDepth yourself.
	 */
	float ClampedEndzoneDepth() const;

	/**
	 * Half extent of an endzone along Y.
	 *
	 * An endzone spans the FULL WIDTH of the field, sideline to sideline - the trigger volume, the
	 * tinted floor patch, the goal line and the gate all use this one function, so what the player
	 * sees and what actually scores cannot drift apart. Do not reintroduce a partial-width zone: a
	 * carrier who crosses the goal line out by a sideline and does not score is indistinguishable
	 * from a broken trigger.
	 */
	float EndzoneHalfWidth() const { return HalfWidth(); }

	/**
	 * World-space box of the endzone at @p EndSign (-1 for the -X end, +1 for the +X end).
	 *
	 * Exactly the volume the ATraceEndzone trigger occupies, exposed so that anything deriving
	 * geometry from an endzone (respawn pads, bot targeting, debug draw) reads the real box rather
	 * than reconstructing it from EndzoneDepth and getting the width wrong.
	 */
	FBox GetEndzoneBounds(float EndSign) const;

	// --- THE POCKET BEHIND THE GOAL (spec v28 §8) -------------------------------------------------
	//
	// "Set the spawns back behind the goals ... just move the wall further back and put the spawn
	// area there."
	//
	// The three functions below are the ONE answer to "where is behind the goal", and they exist as a
	// public surface because ATraceGameMode builds a second, deeper set of respawn pads out of the
	// same band (BuildEndzoneSpawnPads) and the two must not derive it independently. That is not a
	// hypothetical: the endzone-depth clamp used to live in three copies, and when one of them
	// disagreed the pads landed on the centre dais. Same shape of bug, same cure - one function.

	/**
	 * |X| of the goal plane, i.e. the goal line the hoop hangs on. HalfLength() minus the pocket.
	 *
	 * THE anchor for the whole end of the field: the hoop, its two ramps, the scoring slab, the goal
	 * line paint, the approach lane and the spawn band are all measured from here, so moving the end
	 * wall (EndzoneDepth / FieldLength) moves the pocket and leaves the goal exactly where it plays.
	 */
	float GetGoalPlaneX() const;

	/**
	 * *** THE STANDING RULE, CHECKED AT STARTUP: does the gun still reach across this arena? ***
	 *
	 * UTraceSettings::HitscanRange is DERIVED from FieldLength and FieldWidth - it must span the
	 * diagonal or shots expire in mid-air short of a visible target - and it has been left behind by
	 * a field lengthening TWICE (spec v4 §3 and spec v28 §8), both times with the pairing rule
	 * written in a comment beside the value. This runs from EnsureBuilt() on BOTH the procedural and
	 * the baked path, so the mismatch reaches the log of every match instead of waiting for somebody
	 * to think of running a console command.
	 *
	 * It WARNS and does not clamp: quietly raising a designer's ini value would make
	 * Config/DefaultGame.ini stop being the authority it is documented to be. See the .cpp.
	 */
	void WarnIfHitscanRangeIsShort() const;

	/**
	 * THE SIDE RAMPS' DESIGN, RE-CHECKED AGAINST THE LIVE SURF BAND ON EVERY BAKED STARTUP.
	 *
	 * The concave side ramps (SM_SideRampConcave, the owner's parabola swept along both sidelines)
	 * are the ONLY structure in this project whose whole point is that one continuous surface is
	 * walkable at its bottom and surfable at its top. That claim is a claim about two numbers a
	 * designer can move from an .ini — WalkableFloorZ and SurfMinNormalZ — and a mesh cannot notice
	 * when they move. TraceSideRampProfile.h asserts the shipped profile against the band AT BUILD
	 * TIME; this is the other half, and it is here for exactly the reason WarnIfHitscanRangeIsShort()
	 * above it is here: a pairing rule that only lives in a comment gets broken.
	 *
	 * Baked path only. The ramps are hand-placed actors on /Game/Maps/Arena_Baked; the procedural map
	 * has nothing on its side walls, so running this there would be a warning about geometry that is
	 * not present.
	 */
	void WarnIfSideRampProfileIsOutOfBand() const;

	/**
	 * World-space box of the playable pocket behind the goal at @p EndSign: goal plane to end wall,
	 * sideline to sideline, floor to wall top.
	 *
	 * Identical to GetEndzoneBounds() by construction, because in a hockey end they ARE the same
	 * region - see EndzoneDepth. It has its own name because "the endzone" and "behind the goal" are
	 * different questions to a reader even when they have the same answer.
	 */
	FBox GetSpawnPocketBounds(float EndSign) const;

	/**
	 * Signed X of a spawn line inside the pocket at @p EndSign. @p Alpha 0 puts it just behind the
	 * foot of the back approach ramp, 1 just short of the end wall's pawn standoff; it is clamped.
	 *
	 * WHY A BAND AND NOT A NUMBER. Both ends of it are derived - the ramp foot from GoalRampRun(),
	 * the wall from the standoff and the capsule - so a pad placed at an Alpha stays out of the ramp
	 * and out of the wall however the goal, the pocket or the pawn is retuned. A fraction of the
	 * POCKET would not: at a shallower pocket the ramp would eat the near half of it and 0.35 would
	 * be inside the slope.
	 */
	float GetSpawnLineX(float EndSign, float Alpha) const;

	// --- MODE B: goals (spec v4 section 7) --------------------------------------------------------

	/**
	 * Half extent of a GOAL along Y, i.e. half the goal mouth.
	 *
	 * UTraceSettings::GoalWidthFieldFraction of the FULL field width, halved - 0.2083 by default
	 * since spec v5 section 4 shrank it, so 1000 uu either side of the centre line on the 9600 uu
	 * field (a 2000 uu mouth, down from 3200). Read through this
	 * function everywhere, exactly as EndzoneHalfWidth() is: the trigger box, the posts, the crossbar
	 * and the mouth patch all measure themselves against it, so what scores and what is painted are
	 * the same rectangle by construction.
	 *
	 * Clamped so a silly fraction can neither produce a zero-width goal nor a full-width one (which
	 * would silently turn mode B back into mode A with a shorter ceiling).
	 */
	float GoalHalfWidth() const;

	/**
	 * UTraceSettings::GoalHeightUU, clamped to the wall height.
	 *
	 * WHAT IT USED TO MEAN, AND WHAT IT MEANS NOW. Until spec v6 section 4.3 this was the height of a
	 * goal that stood on the floor between the goal line and the end wall, with a crossbar on top of
	 * it. There is no such goal any more - the goal is a ring in the wall, sized by GoalRingRadius()
	 * and placed by GoalRingClearanceZ(). Rather than leave a settings slider that moves nothing (a
	 * dead knob is worse than no knob), it is now the dial on the GOAL APPROACH RAMP: see
	 * GoalRampTopZ(), which clamps it to one step below the hoop. At its shipped 440 the clamp wins
	 * and the ramp sits exactly where the ring puts it; lower it and the run-up gets lower, which
	 * makes carrying the Core through the hoop harder; 0 removes the ramp.
	 */
	float ClampedGoalHeight() const;

	/**
	 * World-space box of the GOAL at @p EndSign (-1 for the -X end, +1 for the +X end).
	 *
	 * Goal line to end wall along X (the same depth as the endzone, so a Core thrown to the back of
	 * the net still counts), +/- GoalHalfWidth() along Y, and floor to ClampedGoalHeight() in Z.
	 */
	FBox GetGoalBounds(float EndSign) const;

	// --- MODE B, SPEC v6 §4.3 + SPEC v28 §8: the goal is a FREE-STANDING RING ----------------------
	//
	// v6 verbatim: "Raise the goals 1.5x player height from the ground, place them into the back
	// walls, and make them circular."
	//
	// v28 §8 verbatim: "Set the spawns back behind the goals. Structure the ends of the field kind of
	// like a hockey field, where you can play behind the goals. Keep the goals the same, raised in
	// the air floating, just move the wall further back and put the spawn area there. Allow goals to
	// be scored through either side of the goal."
	//
	// WHAT v28 CHANGED, AND WHAT IT DELIBERATELY DID NOT. The mouth is IDENTICAL: same 2000 uu
	// diameter, same 1.5-player-height clearance under it, same centre height, same scoring disc.
	// What moved is the wall behind it - out by ClampedEndzoneDepth() - so the hoop no longer sits in
	// a hole in the wall. It hangs unsupported on the goal line with playable floor on BOTH sides,
	// which is what "raised in the air floating" now literally means, and it is why:
	//
	//   * the four wall panels and the alcove that used to frame it are GONE. There is no wall there
	//     any more. The end wall is a plain slab again, in both scoring modes, and is no longer
	//     mode-tagged (it was only tagged because mode B had to replace it with a perforated copy).
	//   * the annulus that closes the square opening down to a circle is now clamped so the whole
	//     hoop CLEARS THE FLOOR - see GoalRingOuterRadius(). Left at its old 1.55x the bottom of the
	//     ring would have been 286 uu underground, which is not floating, it is planted.
	//   * the run-up ramp is built on BOTH faces (see GoalRampRun), and the neon rim on both faces,
	//     because either side is now an approach.
	//   * the scoring slab STRADDLES the ring plane instead of sitting in front of it, which is the
	//     whole of "goals count from either side": every test that scores - ATraceEndzone's disc
	//     test, its 10 Hz poll, ATraceCore's swept crossing test, ATraceGameMode's possession-change
	//     test - already worked off the plane and the disc without caring about direction. They were
	//     never one-directional in ARITHMETIC; the GEOMETRY was, because half of the volume was
	//     buried in a wall. Moving the box is the fix, and it is why there is no second case
	//     anywhere for "scored from behind".
	//
	// WHAT REPLACED WHAT. The v4/v5 goal was a box standing on the floor between the goal line and
	// the end wall, framed by two posts and a crossbar. All of that is gone. In its place each end
	// wall is REBUILT WITH A HOLE IN IT - four panels around a square opening plus a thick neon
	// annulus that closes the opening down to a circle - and the scoring volume is a shallow slab
	// across the mouth of that hole, tested against the disc rather than against its corners
	// (ATraceEndzone::ConfigureRing).
	//
	// THE ONE PLACE THE SPEC CONTRADICTS ITSELF, and how it is resolved. Spec v6 §4.3 asks for the
	// goal CENTRE at 1.5 player heights (264 uu) AND for a 2000 uu diameter. A 1000 uu radius about a
	// 264 uu centre puts 736 uu of the hoop underneath the floor: the ring would not be "raised off
	// the ground" at all, it would be buried in it, which defeats the verbatim instruction the
	// numbers were derived from. The diameter is the number that decides whether the goal is
	// throwable at, so the diameter is kept and the 1.5 player heights are applied to the BOTTOM of
	// the hoop - the hoop clears the floor by exactly 1.5 player heights and its centre sits at
	// 264 + 1000 = 1264 uu. Both halves are dials (GoalRingRaisePlayerHeights, and the diameter via
	// UTraceSettings::GoalWidthFieldFraction), and the resolved numbers are logged at Display so the
	// reading is visible rather than buried here.

	/**
	 * How far the BOTTOM of the ring clears the floor, in player heights. Spec v6 §4.3's
	 * "1.5x player height from the ground" - 264 uu against the 176 uu capsule.
	 *
	 * Clamped at use so the hoop can never be pushed through the floor or through the wall top.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float GoalRingRaisePlayerHeights = 1.5f;

	/**
	 * TOTAL depth of the scoring slab, uu, CENTRED ON THE RING PLANE (spec v28 §8). The volume a
	 * carrier has to be INSIDE to have "carried it through the ring".
	 *
	 * 320 -> 640, and the number doubled because its MEANING changed rather than its reach: it used
	 * to be measured from the wall plane inward, so 320 uu bought 320 uu of field-side slab and
	 * nothing behind (there was nothing behind - it was wall). It is now half either side, so each
	 * side keeps exactly the 320 uu it always had and the far side gains the same. Halve it and you
	 * halve BOTH approaches.
	 *
	 * Not zero, because a plane cannot contain a pawn: at 320 uu a side a carrier standing at the top
	 * of either approach ramp with their capsule against the hoop is inside it, and a Core crossing
	 * the plane is caught by the swept test in ATraceCore whatever the frame rate. GoalSlabHalfDepth()
	 * applies a floor to it for exactly that reason - see there.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float GoalRingDepth = 640.f;

	/**
	 * Run-up ramp at each ring, as a fraction of the ring's floor clearance -> how long the slope is.
	 * The ramp is what makes "carrying the core through the goal" reachable at all now that the mouth
	 * is 264 uu off the ground: a pawn's own jump apex leaves its origin ~50 uu short of the bottom
	 * of the hoop, so without a ramp the carry-in path would be dead and only throwing would score.
	 * Set to 0 to remove the ramps (and with them, in practice, the carry-in goal).
	 *
	 * SPEC v28 §8: there are TWO of them now, one on each face, because the pocket behind the goal is
	 * playable and a carrier coming round the back has to be able to reach the mouth on the same
	 * terms as one coming up the pitch. They are the same ramp mirrored, not a special case - one
	 * loop over the two face signs in BuildGoalRing.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float GoalRampRunPerRise = 4.2f;

	/**
	 * How far the BOTTOM OF THE ANNULUS clears the floor, in player heights (spec v28 §8).
	 *
	 * The hoop is now free-standing, so the ring structure has a bottom that a player can see, and
	 * "raised in the air floating" is a claim about THAT edge rather than about the mouth. The
	 * annulus is therefore no longer 1.55 x the mouth radius unconditionally: it is whatever fits
	 * between the mouth and this clearance. See GoalRingOuterRadius() for the arithmetic and for what
	 * it costs (a thinner ring band at the shipped numbers: 204 uu instead of 550).
	 *
	 * In player heights, like every other vertical number in this arena, so shrinking the capsule
	 * lowers the hoop AND the gap under it together instead of pushing the ring into the floor.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena", meta = (ClampMin = "0.0"))
	float GoalRingFloatPlayerHeights = 0.34f;

	/** Radius of the ring mouth, uu. Half of GoalHalfWidth()'s mouth, i.e. 1000 by default. */
	float GoalRingRadius() const;

	/**
	 * Outer radius of the ring's structural annulus, uu - the outside edge of the hoop you can see.
	 *
	 * SPEC v28 §8. The annulus wants to be GoalRingOuterScale x the mouth (1550 uu), which was free
	 * while the ring was set into a wall: everything below the floor line was simply buried in the
	 * wall panel under the opening. A FREE-STANDING hoop has no such licence - at 1550 its bottom
	 * would be 286 uu UNDERGROUND, and a ring growing out of the floor is not the floating goal the
	 * spec asks for. So the band is clamped to whatever fits above
	 * GoalRingFloatPlayerHeights x a player height, which on the shipped numbers is 204 uu of band
	 * (outer radius 1204) with the bottom of the hoop 60 uu off the deck.
	 *
	 * The MOUTH is untouched by this - only the frame around it gets thinner - which is what keeps
	 * "keep the goals the same" true where it counts: what scores is GoalRingRadius().
	 */
	float GoalRingOuterRadius() const;

	/** Half the ring's depth along the field axis, uu. The hoop keeps the thickness of the wall it used to live in. */
	float GoalRingHalfThickness() const;

	/** Height of the BOTTOM of the hoop above the floor, uu. GoalRingRaisePlayerHeights x a player. */
	float GoalRingClearanceZ() const;

	/** Height of the CENTRE of the hoop above the floor, uu. Clearance + radius, clamped to the wall. */
	float GoalRingCentreZ() const;

	/** Top of the approach ramps at the ring, uu. One step below the hoop, or 0 with no ramp. */
	float GoalRampTopZ() const;

	/** Horizontal run of ONE approach ramp, uu, from the ring's face out to where it meets the floor. */
	float GoalRampRun() const;

	/**
	 * Half the depth of the scoring slab, uu, measured out from the ring plane along the field axis.
	 *
	 * GoalRingDepth halved, with a FLOOR under it, and the floor is the part worth reading: a carrier
	 * standing at the top of a ramp is held off the ring plane by the hoop's own half thickness plus
	 * their capsule radius, so a slab shallower than that could be stood in front of and never
	 * entered. Anything below that is a goal that cannot be carried into, which is a silent rule
	 * change dressed up as a tuning value.
	 */
	float GoalSlabHalfDepth() const;

	/** World-space centre of the ring mouth at @p EndSign. */
	FVector GetGoalRingCentre(float EndSign) const;

	/**
	 * The box that actually scores at @p EndSign in the mode currently armed - the goal in mode B,
	 * the endzone in mode A.
	 *
	 * This is the one anything mode-agnostic should call: bot goal-seeking, throw aiming, debug
	 * draw. Asking for GetEndzoneBounds() in mode B is how a bot ends up running at a target that
	 * cannot score.
	 */
	FBox GetScoringBounds(float EndSign) const;

	/** The scoring shape the arena is currently presenting. Told to it; never inferred here. */
	ETraceScoringMode GetScoringMode() const { return ScoringMode; }

	/**
	 * Presents @p NewMode: arms that mode's scoring volumes, shows its furniture, and hides and
	 * disarms the other mode's.
	 *
	 * Idempotent, cheap (a visibility and collision push over ~80 components and 4 actors), and legal
	 * at any point in a match on any machine - no rebuild, no restart. Safe to call before the arena
	 * is built: the mode is remembered and applied at the end of the build.
	 */
	void ApplyScoringMode(ETraceScoringMode NewMode);

	/**
	 * Finds this world's builder and switches it. Called from BOTH sides of the network for the same
	 * reason ApplyTeamSidesInWorld is: the builder is not replicated, so the server and every client
	 * has to be driven independently (the game mode on publish, ATraceGameState::OnRep on clients).
	 */
	static void ApplyScoringModeInWorld(const UWorld* World, ETraceScoringMode NewMode);

	//~ End mode B surface

	/**
	 * Builds the arena now if it has not been built yet. Idempotent, and legal to call before
	 * BeginPlay — which is the whole point: ATraceGameMode::PreInitializeComponents has to get the
	 * player starts into the world before AGameModeBase::Login runs FindPlayerStart, and that is two
	 * steps earlier than any BeginPlay.
	 *
	 * ON A BAKED LEVEL IT BUILDS NOTHING (spec v15 §1.5). See bLevelIsPreBaked: the geometry is
	 * already in the .umap, so this adopts it instead of constructing a second copy on top of it.
	 */
	void EnsureBuilt();

	// --- THE BAKED LEVEL (spec v15 §1) -------------------------------------------------------------
	//
	// /Game/Maps/Arena_Baked is this builder run once and written down: every piece of geometry
	// exploded into an individually selectable ATraceBakedPiece actor with a readable label, the
	// gameplay actors placed for real, and the materials promoted to committed assets under
	// /Game/Trace/Materials. Scripts/bake-arena.sh produces it; nothing about the procedural path
	// changed, and /Game/Maps/Arena still builds itself at BeginPlay exactly as it always did.
	//
	// THE DOUBLE-BUILD PROBLEM, AND WHERE THE FIX LIVES. ATraceGameMode finds this actor (or spawns
	// one) and calls EnsureBuilt() from PreInitializeComponents, whatever level is loaded. On a baked
	// level that would put a second floor, a second set of walls and a second pair of endzone
	// triggers on top of the ones already in the map. The check therefore lives HERE, in the builder,
	// not in the game mode: the game mode has no business knowing which levels are baked, and a level
	// that gains a bake later must not need a game-mode edit to go with it.

	/**
	 * True on the builder that the bake leaves behind in /Game/Maps/Arena_Baked.
	 *
	 * WHY THE BUILDER IS STILL IN THE BAKED LEVEL AT ALL, given it builds nothing there. Because it
	 * is the arena's ORACLE as much as its constructor: GetFieldBounds(), GetCoreSpawnLocation(),
	 * GetScoringBounds(), ClampedEndzoneDepth() and GoalRingCentre() are pure functions of the layout
	 * properties above, and the game mode, the Core, the bots and the half-time switch all ask them.
	 * Deleting it from the baked level would leave that geometry unanswerable — the Core would spawn
	 * at ATraceGameMode's fallback location and the bots would steer inside a default box.
	 *
	 * It is also what adopts the baked level's endzones, lights and post-process volume back into the
	 * live state ApplyScoringMode / ApplyTeamSides / ApplyFidelity operate on. See AdoptBakedArena.
	 *
	 * A second, independent trigger exists so that a designer who deletes and re-places the builder
	 * does not silently get a double-built level: any actor in the world carrying the
	 * TraceBakedArenaTag() tag also switches the build off. The bake stamps that tag on every piece
	 * it emits, so the level is self-describing.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Bake")
	bool bLevelIsPreBaked = false;

	/** The actor tag every baked piece carries. Also the level's "I am already built" marker. */
	static FName TraceBakedArenaTag();

	/** True when this builder must not construct geometry: its own flag, or a marker in the level. */
	bool IsLevelPreBaked() const;

#if WITH_EDITOR
	/**
	 * Runs the whole build ONCE and writes it into the current editor level as real, editable,
	 * saveable actors. The entry point Scripts/bake-arena.py drives. Editor worlds only.
	 *
	 * WHAT IT EMITS
	 *   * one ATraceBakedPiece per logical piece — a wall, a cover block, a pylon, a bank terrace —
	 *     carrying that piece's mesh components, its BlockAll box and its pawn standoff shell, and
	 *     labelled with what it is (`Wall_PosY`, `CoverBlock_014`), never `StaticMeshActor_417`;
	 *   * one ATraceBakedPiece per instanced RUN for the pieces there are too many of to be worth a
	 *     click each — the wall cove's 5 uu shells, the floor grid — as a single
	 *     UInstancedStaticMeshComponent. That is BakeMaxPiecesPerName's job and the count is logged;
	 *   * the gameplay actors as ordinary placed actors: both pairs of ATraceEndzone, ten
	 *     ATraceTeamPlayerStarts, the three directional lights, the sky light, the atmosphere, the
	 *     height fog, the post-process volume and the floor-lamp lattice as real APointLights;
	 *   * committed UMaterialInstanceConstants under /Game/Trace/Materials/Instances for every
	 *     distinct tint the build asked for, because a .umap may not reference Content/Generated/
	 *     (it is gitignored, so the map would be broken on every other machine).
	 *
	 * It then sets bLevelIsPreBaked on itself, so saving the level is all that is left to do.
	 *
	 * NOT IDEMPOTENT ACROSS RUNS in the sense of cleaning up after a previous bake: run it into a
	 * fresh empty level, which is what Scripts/bake-arena.py does. Running it twice into one level
	 * gives you two arenas, exactly as pressing Build Preview twice would if it did not clear first.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Trace|Bake")
	void BakeArenaIntoLevel();

	/**
	 * Places the Core spawn marker in this level if it does not already have one, at whatever
	 * GetCoreSpawnLocation() currently answers. Editor only, idempotent, safe to press twice.
	 *
	 * WHY THIS IS A SEPARATE BUTTON AND NOT JUST PART OF A BAKE (spec v17 §2). A full re-bake
	 * rewrites all ~572 external actor packages, which is ~5.4 MB of LFS churn and throws away every
	 * hand edit anybody has made to the level. Adding the one actor spec v15 §1.4 asked for should
	 * not cost that. So: press this on the builder in an already-baked level, save, done — one new
	 * package, nothing else touched. Scripts/bake-arena.sh --add-core-spawn does exactly that
	 * headlessly, and a fresh bake places the marker on its own.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Trace|Bake")
	void EnsureCoreSpawnActor();
#endif

	// The two bake dials are OUTSIDE the #if WITH_EDITOR above on purpose: UnrealHeaderTool rejects a
	// UPROPERTY wrapped in WITH_EDITOR ("use WITH_EDITORONLY_DATA instead"), and stripping them from a
	// packaged build would buy two floats and cost the ability to read, in a shipped build's Details
	// panel, the numbers the level in front of you was baked with.

	/**
	 * How many separate ATraceBakedPiece actors one debug name may produce before the bake stops
	 * giving that name's blocks loose static mesh components and batches every one of them into
	 * instanced components instead.
	 *
	 * IT DOES NOT CHANGE THE ACTOR COUNT. Every piece is still its own actor, still labelled, still
	 * individually clickable — this dial is about what is INSIDE the piece. The cover field is the
	 * case it was written for: 152 cover blocks, each of which would otherwise contribute two or
	 * three loose UStaticMeshComponents on top of its batched trim. Spec v7 §8 rebuilt the whole
	 * arena on instanced meshes because ~1450 loose components cost 1.8-3.1 ms of draw submission,
	 * and a bake that handed that back would be a regression dressed up as a migration.
	 *
	 * The bake logs which names crossed the line and the full per-name census, so the trade is
	 * visible rather than assumed.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Bake", meta = (ClampMin = "1"))
	int32 BakeMaxPiecesPerName = 48;

	/**
	 * How far apart two blocks sharing a debug name may be, in the horizontal plane, and still be
	 * treated as one piece.
	 *
	 * This is what turns a flat list of blocks back into THINGS without threading a "which piece am I
	 * part of" argument through forty call sites. AddNeonBlock's twelve components all sit inside the
	 * block's own footprint (the widest is the standoff shell, 26 uu proud on each side); the next
	 * cover block is thousands of uu away. 96 uu clears the former with room to spare and cannot
	 * reach the latter.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Bake", meta = (ClampMin = "0.0"))
	float BakePieceGroupSlack = 96.f;

	// --- FIDELITY, GATED BEHIND QUALITY (spec v11 §3) ---------------------------------------------
	//
	// WHAT THIS IS. Everything expensive the arena can render - Lumen GI and reflections, ambient
	// occlusion, cascaded shadows, FFT bloom, screen-space reflection quality and the floor-lamp
	// lattice - is written into the arena's own post-process volume and lights by ONE function,
	// ApplyFidelity(), from the engine's scalability levels. Nothing here is a build-time decision:
	// the arena is built once and re-tuned live, so changing Shadows from Low to Epic in the video
	// settings menu costs a few property writes rather than a rebuild of 1240 blocks.
	//
	// WHICH SCALABILITY GROUP DRIVES WHAT, and this mapping is deliberate - it is exactly the row
	// list spec v11 §2.5 asks the settings UI for, so a player who turns "Reflections" down turns
	// down the thing labelled reflections and nothing else:
	//
	//   Global Illumination  -> Lumen GI. Off below High. THE expensive one, and see the gate below.
	//   Reflections          -> the floor mirror: OFF at Low, SSR at Medium/High, Lumen at Epic.
	//   Shadows              -> the key light's cascades; NO shadow pass at all at Low.
	//   Post Processing      -> ambient occlusion, and FFT (convolution) bloom at Epic only.
	//   Effects              -> the floor-lamp lattice's radius, i.e. its screen footprint.
	//
	// BLOOM IS NEVER SWITCHED OFF at any tier, only changed in method. It is what makes an emissive
	// strip read as neon rather than as a flat coloured rectangle, and the arena has no other lighting
	// idea. Turning it off would save frames and delete the art direction.
	//
	// LUMEN CANNOT ARM ON THIS PROJECT AS IT SHIPS, AND THAT IS DELIBERATE - it is the one place this
	// ladder does not simply obey the quality level, so read this before wondering why Epic looks the
	// same as High.
	//
	// MEASURED: the engine's desktop scalability defaults are Epic on EVERY group (a headless run
	// logs "scalability: ... GI=3 Reflections=3 PostProcess=3 ..."). A ladder that just followed
	// GlobalIlluminationQuality would therefore turn Lumen on for everybody by DEFAULT, which spec
	// v11 §0 forbids outright - and it would turn it on over a project whose
	// r.GenerateMeshDistanceFields is False, so software ray tracing would have had nothing to trace
	// against: full price, no light. ApplyFidelity holds Lumen off unless distance fields are
	// enabled, which makes that project setting the explicit, deliberate opt-in to the whole feature.
	// r.GenerateMeshDistanceFields lives in Config/DefaultEngine.ini, which this file does not own;
	// the request for it, and what it buys and costs, is in this pass's report.

	/**
	 * Pushes the current scalability levels into this arena's post-process volume and lights.
	 *
	 * Idempotent and cheap - a few dozen property writes plus one render-state dirty per light - and
	 * safe to call at any time, including before the arena is built (it no-ops). Called at the end of
	 * BuildArena, from a Scalability::OnScalabilitySettingsChanged handler, and from the
	 * Trace.Arena.Fidelity* console variables.
	 */
	void ApplyFidelity();

	/** Finds this world's builder and re-applies its fidelity. Null-safe; no-op with no builder. */
	static void ApplyFidelityInWorld(const UWorld* World);

	/** One line naming every fidelity feature and the tier it is currently at. For the log and menu. */
	FString DescribeFidelity() const;

#if !UE_BUILD_SHIPPING
	/**
	 * Tears the arena down and builds it again in place. FOR MEASUREMENT ONLY - see spec v7 §8.
	 *
	 * It exists because comparing the instanced geometry path against the legacy one across two
	 * SEPARATE PROCESSES is not a comparison on a machine that has anything else running on it: the
	 * first attempt at this measurement was invalidated twice by another headless run starting up
	 * inside the sample window. Rebuilding in one process lets Trace.Arena.PerfAB interleave the arms,
	 * which spreads any outside interference evenly over both instead of dumping it on whichever arm
	 * was unlucky.
	 *
	 * NOT A LIVE-TUNING FACILITY. It destroys and respawns the endzone triggers and the player starts
	 * and resets the painted side assignment; the pawns standing on the floor survive only because the
	 * collision is rebuilt in the same frame it is destroyed.
	 */
	void RebuildForMeasurement();
#endif

	//~ End contract surface

	//~ AActor
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor

#if WITH_EDITOR
	// ---------------------------------------------------------------------------------------------
	// EDITOR PREVIEW - see the arena without pressing Play
	//
	// THE PROBLEM. /Game/Maps/Arena is a deliberately EMPTY level (build contract section 2: the
	// project ships no authored level .uasset) and everything you see in a match is built from C++ at
	// BeginPlay. Open the map in the editor and you therefore get a blank grid, which reads as a
	// broken project rather than as a design decision, and it means the layout cannot be inspected,
	// measured or screenshotted without launching the game.
	//
	// THE WORKFLOW, two clicks:
	//   1. Drag an ATraceArenaBuilder from the Place Actors panel into the level (put it at the
	//      origin - the whole arena is built in this actor's local space, so the builder's transform
	//      IS the arena's transform).
	//   2. With it selected, press "Build Preview In Editor" in the Details panel under Trace|Preview.
	//      The full arena - floor, walls, dais, wings, rails, cover, flanks, endzones, the lighting
	//      rig, fog and post process - appears in the viewport immediately.
	//   Press "Clear Preview In Editor" to take it all away again. Editing any property above (field
	//   size, wall height, endzone depth, lighting) while a preview is up rebuilds it automatically.
	//
	// WHY IT CANNOT LEAK. Two independent guarantees, because "the preview got saved into the map"
	// would double the arena at runtime - two floors, two sets of endzone triggers, two of everything:
	//   1. Everything the preview creates is RF_Transient (BuiltObjectFlags() below stamps every
	//      component, every MID and every spawned actor), and transient objects are never written to
	//      a .umap. Saving the level with a preview up saves the builder actor and nothing else.
	//   2. BuildArena() tears any surviving preview down before it builds for real, so even a preview
	//      that somehow reached a play session cannot be there twice.
	// Both buttons refuse to run outside an editor world, so neither can touch a live match.
	// ---------------------------------------------------------------------------------------------

	/** Builds the whole arena into the editor viewport, transiently. Idempotent: rebuilds if shown. */
	UFUNCTION(CallInEditor, Category = "Trace|Preview")
	void BuildPreviewInEditor();

	/** Removes the editor preview. Safe to press when there is nothing to remove. */
	UFUNCTION(CallInEditor, Category = "Trace|Preview")
	void ClearPreviewInEditor();

	/** Keeps a live preview in step with the layout properties above. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// ---------------------------------------------------------------------------------------------
	// Layout. All values in unreal units, all local to this actor's transform, so the whole arena
	// can be moved by moving the builder. Editable for tuning via a Blueprint subclass assigned to
	// ATraceGameMode::ArenaBuilderClass - see the net note in the class comment before changing any
	// of them at runtime.
	// ---------------------------------------------------------------------------------------------

	/**
	 * Length of the field along X, WALL TO WALL. Since spec v28 §8 that is NOT goal to goal - see
	 * below.
	 *
	 * 24000 -> 33600 for spec v4 section 3, verbatim: "Lengthen the map to a 3.5:1 ratio, adjusting
	 * the structures to match." 33600 : 9600 is exactly 3.5 : 1, and [ASSUMPTION] "lengthen" grows
	 * the long axis rather than narrowing the short one.
	 *
	 * SPEC v28 §8 - HOCKEY ENDS. 33600 -> 38400, and the whole of the extra 4800 is the two POCKETS
	 * behind the goals: "just move the wall further back and put the spawn area there. You can
	 * slightly lengthen the map for this." The goal plane did not move. It is still at |X| = 16800,
	 * exactly where the end wall used to stand, so GOAL TO GOAL IS STILL 33600 uu and every tuned
	 * distance on the pitch - the cover scatter, the run from the centre diamond to a hoop, the
	 * 42-second full-field run - is unchanged. What changed is that there is now
	 * ClampedEndzoneDepth() (2400 uu) of playable floor BEHIND each hoop, with the spawn fan in it.
	 *
	 * So: FieldLength = 33600 (goal to goal) + 2 x EndzoneDepth (the two pockets). Change either and
	 * the other has to follow or the goals stop being 33600 apart; that pairing is the one thing in
	 * this file that is not self-deriving, and it is stated here because there is nowhere else to
	 * state it - the goal plane is HalfLength() - ClampedEndzoneDepth() by construction.
	 *
	 * The layout scales with this: the cover scatter, the corner banks, the pylons and the endzone
	 * gates are all placed at fractions of the half length, so 38400 is a tuning value rather than a
	 * load-bearing constant. Do not drop it below ~12000 or the centre diamond and the two spawn
	 * lines start to overlap.
	 *
	 * THE COST, STATED PLAINLY: at WalkSpeed 800 a wall-to-wall run is now 48 seconds (42 of them
	 * goal to goal, 3 in each pocket). UTraceSettings::HitscanRange has to clear the field DIAGONAL
	 * (38400 x 9600 -> 39581 uu) and DOES: Config/DefaultGame.ini ships HitscanRange=39600, raised
	 * from the 36000 that covered the old 33600 field when the pockets landed, and
	 * WarnIfHitscanRangeIsShort() re-checks the pairing in the log of every match — so a future
	 * resize here cannot silently strand the long diagonal again.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float FieldLength = 38400.f;

	/**
	 * Width of the field along Y (sideline to sideline). Layout scales with this too.
	 *
	 * 12000 -> 9600 for spec v3 section 7: the collaborator's sketch is drawn at LENGTH : WIDTH =
	 * 2.5 : 1, and the length is the dimension worth keeping (it is what makes a carry a journey).
	 * The narrower field also pulls the flanks back into play - at 12000 the outer thirds were two
	 * black voids that needed a whole subsystem of dressing to fill.
	 *
	 * EVERYTHING derived from this follows automatically: the endzone volumes and their triggers, the
	 * mode-B goal mouths, the spawn fan, GetFieldBounds() (which is what the bots steer inside and
	 * what the half-time side switch measures against), the grid, the flanks and the corner banks.
	 *
	 * THE ONE NUMBER THAT DOES NOT LIVE HERE and must move with these two is
	 * UTraceSettings::HitscanRange, which has to clear the field diagonal. Spec v28 §8 lengthened
	 * the field to 38400 x 9600 for the two hockey pockets (a 39581 uu diagonal) and the range
	 * followed: Config/DefaultGame.ini now ships HitscanRange=39600 (the ini wins over the
	 * UTraceSettings default), clearing the diagonal with 19 uu to spare. The pairing is guarded at
	 * runtime by WarnIfHitscanRangeIsShort(), so a field resize shows up in every match log rather
	 * than as shots dying short of targets the player can plainly see.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float FieldWidth = 9600.f;

	/**
	 * Wall height. Tall on purpose: on a 33600 uu field a 700 uu wall is a kerb, and the walls are
	 * the main thing standing between the camera and an empty black sky.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float WallHeight = 2600.f;

	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float WallThickness = 200.f;

	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float FloorThickness = 120.f;

	/**
	 * How far each endzone reaches in from its end wall, i.e. its size along X.
	 *
	 * SINCE SPEC v28 §8 THIS IS THE POCKET BEHIND THE GOAL - the hockey rink's end zone. The goal
	 * plane and the goal line are the same thing (GoalLineX()), the hoop hangs on it, and everything
	 * from there back to the end wall is playable floor with the spawn fan in it. One number, one
	 * region, and the mode-A endzone volume, the mode-A floor paint, the spawn band and "behind the
	 * goal" are all literally the same box - which is why this is the dial for how much room there is
	 * back there rather than a second "PocketDepth" property that could disagree with it.
	 *
	 * THERE IS NO WIDTH DIAL, and that is deliberate: an endzone spans the ENTIRE width of the field,
	 * sideline to sideline, exactly like a real football endzone. See EndzoneHalfWidth().
	 *
	 * Read it through ClampedEndzoneDepth(), never raw - three separate places used to re-clamp this
	 * by hand and one of them getting it wrong is what put both teams' spawn lines on the centre dais.
	 *
	 * Growing it moves the WALL outward, not the goal, as long as FieldLength is grown by twice as
	 * much with it (see FieldLength). At 2400 the pocket holds the 941 uu back ramp, a 1257 uu spawn
	 * band behind it and the corner pylons, with room to run round the hoop on either side.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float EndzoneDepth = 2400.f;

	/** Spacing of the floor grid strips. Clamped so a small value cannot spawn thousands of them. */
	UPROPERTY(EditAnywhere, Category = "Trace|Grid")
	float GridSpacing = 1000.f;

	/** Width of a grid strip (they are thin slabs laid on the floor). */
	UPROPERTY(EditAnywhere, Category = "Trace|Grid")
	float GridStripWidth = 16.f;

	/** Hard cap on grid strips per axis. Guards the component budget against a silly GridSpacing. */
	UPROPERTY(EditAnywhere, Category = "Trace|Grid")
	int32 MaxGridLinesPerAxis = 51;

	/** Master switch for the interior layout (centre diamond, cover scatter, gates). */
	UPROPERTY(EditAnywhere, Category = "Trace|Layout")
	bool bBuildInteriorLayout = true;

	/**
	 * Master switch for the sky dressing (release overhaul, MAP plan §4): the 24-tower skyline ring
	 * beyond the walls, the horizon glow band behind each end, and the goal beacons over the rings.
	 *
	 * Visuals only — BuildSkyline is called inside the bBuildVisuals gate, builds no collision and
	 * casts no shadows, so a dedicated server has no sky at all and nothing here can affect play.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Layout")
	bool bBuildSkyline = true;

	// --- Corner banks (the sketch's green arrows) ------------------------------------------------
	//
	// Four banks, one per quadrant, raised along the long edges and sweeping DOWN toward the middle
	// of the field. Read the sketch as a shallow stadium bowl: high at the corners, flat in the
	// centre. See BuildCornerBanks for the shape.
	//
	// SPEC v10 SECTION 9 - THEY ARE NO LONGER TERRACED. The bank is still built from nested
	// axis-aligned boxes, for the reason BuildCornerBanks gives (this project's bots have no navmesh
	// and a pitched collider met side-on is a wall to them), but the boxes a PLAYER STANDS ON and the
	// boxes a player LOOKS AT are now two different sets at two different resolutions: 20 uu risers
	// for the invisible colliders, 5 uu for the drawn shell. A 5 uu riser is 2.8% of a player's
	// height and does not resolve as a step at any distance in this arena.

	/** Master switch for the four corner banks. */
	UPROPERTY(EditAnywhere, Category = "Trace|Banks")
	bool bBuildCornerBanks = true;

	/**
	 * How far a bank reaches in from its sideline, i.e. the width of the sloped strip along Y.
	 *
	 * This is the number that decides how much FLAT playfield is left: the flat centre is
	 * FieldWidth - 2 * BankDepth wide, so 1500 on a 9600 field leaves 6600 uu of flat ground -
	 * still wider than the whole original 8000 x 4000 arena. Push it much past 2000 and the two
	 * banks start eating the routes the cover scatter is built around.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Banks", meta = (ClampMin = "0.0"))
	float BankDepth = 1500.f;

	/**
	 * Height of a bank at its highest terrace, i.e. how deep the bowl is.
	 *
	 * Defaults to 2x player height (352 uu) so it matches the sketch's mid-height structures: high
	 * enough that standing on the bank is genuinely high ground and that a body behind it is
	 * hidden, low enough that it never reads as a wall. The step count is derived from this so that
	 * every riser stays under UCharacterMovementComponent::MaxStepHeight whatever it is set to - and
	 * since spec v10 section 9 the collision riser is half of even that ceiling, so raising this
	 * number adds steps rather than height to any of them. See BuildCornerBanks.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Banks", meta = (ClampMin = "0.0"))
	float BankHeight = 352.f;

	// --- Wall fillets (spec v9 section 10) --------------------------------------------------------
	//
	// "Curve the corners of the arena walls, so that the crosssection of the arena looks something
	// along these lines" - a concave COVE running along the base of every wall instead of a hard 90
	// degree join. Two pieces answer that, because the arena does not have one wall/floor join, it
	// has two:
	//
	//   1. THE CORNER BANKS were already the transition along the side walls, and they were a
	//      STRAIGHT ramp that hit the wall dead square at its crest. BuildCornerBanks now lays its
	//      terraces on a quarter-ellipse instead of on a line, so the bank IS the curve.
	//   2. EVERYWHERE ELSE - both end walls, the endzone stretches of side wall, and the midfield
	//      taper where the bank is only one terrace tall - gets its own small cove from
	//      BuildWallFillets.
	//
	// Both are built by the same profile generator (TraceBuildCoveProfile in the .cpp) under the same
	// three rules, and those rules are what keep the curve WALKABLE and free of false affordances:
	//   riser <= MaxRiser          so UCharacterMovementComponent steps up it without the mover
	//                              ever knowing there was a step (and every riser is far below
	//                              MantleMinHeightUU 55, so none can read as a climbable ledge);
	//   slope <= FilletMaxSlope    so the mantle's top probe, which lands CapsuleRadius*0.5 = 17 uu
	//                              past the face it found, can never find a ledge inside the mantle
	//                              window: the worst it can report is 17 x slope;
	//   inner edge >= 40 uu        so the cove stops at the pawn standoff line and never leaves a
	//                              walkable sliver a body cannot actually reach.
	// A cove is tangent to the wall at its top, so those rules necessarily truncate it. On the
	// shipped geometry the inner-edge rule bites first, at ~45 degrees on the banks and ~52 on the
	// wall fillets; the wall goes vertical from there.
	//
	// SPEC v10 SECTION 9 - "CURVED, NOT TERRACED". v9 answered the note by putting the terraces ON a
	// curve, which fixed the PLAN and left the CROSS-SECTION a visible flight of 39 uu stairs; the
	// user looked at it and asked again. Both pieces are now built TWICE from the same envelope:
	// invisible box colliders at CoveCollisionRiser (half the walkable ceiling, so every Demo 9
	// property is strictly improved and the box-component budget barely moves) and drawn instanced
	// shells at CoveVisualRiser, which is 5 uu and costs no registered primitive at all. The three
	// rules above are stated as SLOPES rather than as absolute treads precisely so that both
	// resolutions describe the same shape - see FilletMaxSlope in the .cpp.

	/** Master switch for the cove along the base of every wall. */
	UPROPERTY(EditAnywhere, Category = "Trace|Walls")
	bool bBuildWallFillets = true;

	/**
	 * How far the cove reaches IN from a wall face, i.e. the horizontal semi-axis of the quarter
	 * ellipse. This is the number that decides how much floor the curve eats.
	 *
	 * 600 checked against everything that stands near a wall on the shipped 33600 x 9600 field:
	 * corner pylons are 720 uu off the side wall, the outer lane stripe 1632, the spawn fan 1920,
	 * the gate towers 300 (the cove is 30 uu tall there, i.e. a floor decal). The wall buttresses
	 * are flush with the wall and 200 deep, so they simply rise out of the cove.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Walls", meta = (ClampMin = "0.0"))
	float WallFilletDepth = 600.f;

	/**
	 * Vertical semi-axis of the cove, i.e. how far up the wall it would reach if it ran all the way
	 * to tangency. The staircase truncates before that, where its inner edge reaches the pawn
	 * standoff line - at 296 that is ~190 uu up with the innermost edge 40 uu off the wall.
	 *
	 * SPEC v10 SECTION 9 CHANGED THE SAMPLING, NOT THE ENVELOPE. Both truncation rules are now
	 * expressed as slopes rather than as absolute treads, so the curve stops in exactly the same
	 * place whether it is sampled at the 20 uu collision riser or the 5 uu drawn one. The number
	 * above still means what it always meant.
	 *
	 * KEPT UNDER THE MODE-B GOAL FURNITURE on the end walls, and that clamp is applied per step at
	 * build time against the real ramp geometry rather than assumed here: the carry-in ramp climbs
	 * to GoalRampTopZ (224 uu) over GoalRampRunPerRise x that, and a cove step poking through it
	 * would put a lip across the mouth of the hoop. See BuildWallFillets.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Walls", meta = (ClampMin = "0.0"))
	float WallFilletHeight = 296.f;

	/**
	 * Master switch for the flank dressing: wall buttresses, the high rails they carry, the light
	 * bridges out to the lane pylons, the outer-lane floor stripes and the corner pylons.
	 *
	 * WHY IT EXISTS. The field was 12000 uu wide and every route worth taking ran down the middle,
	 * so the outer thirds rendered as two black voids - one measured screenshot had an entirely
	 * black left half. Empty space at this scale does not read as "arena", it reads as "unfinished".
	 * Everything this builds is either flush against a side wall or above head height, so it fills
	 * the void without narrowing the lanes or giving the navmesh-less bots a pocket to grind in
	 * (see ATraceBotController).
	 *
	 * Still worth keeping at 9600 wide, and still cheap: the corner banks now occupy the outer 1500
	 * uu of each side, but they are ground, so they fill the BOTTOM of a flank frame and leave the
	 * middle band - the part the high rail and the light bridges answer - exactly as empty as it was.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Layout")
	bool bBuildFlankStructures = true;

	/**
	 * PATCH 28 §5. The four curved SURF RAILS in the outer lanes. See BuildSurfRails().
	 *
	 * Its own switch, like the banks' and the flanks', for the same reason: "the arena with the rails"
	 * and "the arena without them" both have to be lookable-at while the movement feature they exist
	 * for is being tuned, and an A/B of a movement mechanic against the geometry it runs on is worth
	 * one bool.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Layout")
	bool bBuildSurfRails = true;

	// --- THE SIDE-WALL SURF BANDS ------------------------------------------------------------------
	//
	// THE OWNER'S INSTRUCTION: "do NOT add new ramps in the outer lanes. Turn THE BANKS ALONG THE SIDE
	// WALLS into the curved surf ramps." This is that. See BuildSurfBanks() for the construction and
	// SurfBankToeDepth() for where the shape comes from.
	//
	// WHY THE BANK AND NOT A FIFTH RAIL. The collaborator rode the rails and said "the ramps don't work
	// on the end, plus they're not curved they're just angled", and the second half of that is
	// arithmetic rather than taste. A surface is surfable only between the walkable limit (44.77 deg on
	// the shipped movement knobs) and acos(SurfMinNormalZ) (63.26): THE WHOLE BAND IS 18.5 DEGREES WIDE,
	// and with the 2-degree margins the rails already take at both ends a face can bend at most 14.5
	// degrees from toe to crest. No cross-section can read as "curved" inside that, whatever its radius
	// - the sagitta of a 14.5-degree arc is 0.8% of its own chord at ANY radius, so the rails' 763 uu
	// face and this 286 uu one are equally flat-looking as a FRACTION of themselves. Three previous
	// passes reduced the crease BETWEEN facets from 2.76 to 1.38 degrees and never moved total
	// curvature, because total curvature is capped by the band and they were optimising the wrong
	// quantity.
	//
	// WHAT IS NOT CAPPED is curvature about a VERTICAL axis. Rotating a plane about a vertical axis
	// leaves its normal's Z untouched, so a face may turn as far as you like IN PLAN and stay exactly
	// as surfable as it started - a right circular cone has constant slope over its whole surface.
	// That is where the curve in this structure comes from: the run is straight, and each end PEELS OFF
	// THE WALL through a mitred fan of plan segments before its nose delivers the rider to the floor.
	// The fan costs nothing in band margin (checked at build time, not asserted here) and it is the
	// only kind of curvature this geometry can actually show.
	//
	// WHAT IT COSTS, STATED HERE BECAUSE IT IS A GAMEPLAY CHANGE AND NOT AN IMPLEMENTATION DETAIL:
	//   * The bank's WALKABLE cove is truncated earlier - at SurfBankToeDepth() instead of at the 40 uu
	//     pawn standoff - so the walkable crest drops from ~271 uu to ~70. The cove's envelope, its
	//     three stop rules and its generator are all UNCHANGED; it simply stops sooner and the band
	//     continues from there. Below the toe not one box moves. The perimeter is still walkable, still
	//     tangent to the floor and still keeps bots off the wall, but the bank is no longer high ground
	//     you can hide a standing body behind - see SurfBankCrestZ for why that price was paid and what
	//     was measured before paying it.
	//   * The bank's INBOARD X taper goes, so the ride runs the full length of the wall instead of
	//     existing only near the corners. The bowl's goal-line SETBACK is untouched: the bank still
	//     steps back from the endzone and the endzones are still flat and full width.
	//   * The structure's total height becomes the arena's 3.5x class (616 uu), the same as the surf
	//     rails it replaces, and it tops out on a level bench at the buttress line rather than at an
	//     edge.
	//
	// Its own switch for the reason the rails' switch exists: an A/B of a movement mechanic against the
	// geometry it runs on is worth one bool, and this one also has to be comparable against the rails.
	//
	// *** DEFAULTED OFF, AND IT SHIPPED THAT WAY DELIBERATELY. THIS BAND DOES NOT RIDE. ***
	//
	// The idea was sound and the measurements below it are real, but the thing does not work and the
	// honest place to record that is here, next to the switch, rather than in a report nobody reads:
	//   * IT HAS NO ENTRANCE. -TraceSurfApproachTest runs a pawn at it from the floor and scores 0 of
	//     6 rides, four of them stopping dead, against 5 of 6 at 1272-1286 uu/s on the surf rails in
	//     the same binary and session. The band's walkable bench sits at Z 616 and its toe 600 uu up
	//     a terraced bank, so there is nowhere a body can get onto it.
	//   * IT IS NOT CURVED, which was the entire point of building it. GetSurfBankSections emits one
	//     plan-straight 31,700 uu prism plus 2 x 986 uu of turn per wall: 94.1% of the ride is a flat
	//     plane. Player-eye frames show two dead-straight neon lines converging on a vanishing point.
	//   * The premise it was dispatched on does not survive its own arithmetic. The strip between
	//     where the cove stops (44.77 deg) and where the wall goes vertical (63.26 deg) really is the
	//     surf band in ANGLE, but it is 29.9 uu of run and 49.7 uu of slope-length - a fifteenth of
	//     the rail it was meant to replace - and all of it inside the 40 uu pawn standoff shell. The
	//     angular coincidence is real; the space is not.
	// Turning it on costs +124 registered primitives (523 -> 647) for geometry nobody can ride, so it
	// stays off until somebody solves the entrance. The code and its instrument (Trace.Arena.
	// SurfBankProfile, whose control arm caught two defects in itself) are kept because the shape work
	// and the measurements are reusable; the DEFAULT is the claim, and the claim is "not yet".
	UPROPERTY(EditAnywhere, Category = "Trace|Layout")
	bool bBuildSurfBanks = false;

	/**
	 * How far the surf band's crest stands off the side wall, BEYOND the buttress row.
	 *
	 * NOT A TASTE NUMBER. Thirty wall buttresses stand flush against the perimeter walls and are
	 * ButtressDepth (200 uu) deep, from Z = 0 up. A ride surface whose crest reached the wall face
	 * would pass THROUGH every one of them - a 420 uu wide block across the ride every 2520 uu - and
	 * moving the crest to exactly ButtressDepth would leave the buttress face and the bench face
	 * coplanar and z-fighting over 420 x 352 uu apiece. So the crest stops this far short of the
	 * buttress line: the buttresses stand ON the bench the band tops out on, they overhang nothing,
	 * and no two drawn faces are coincident.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Layout", meta = (ClampMin = "0.0"))
	float SurfBankTopClearance = 4.f;

	UPROPERTY(EditAnywhere, Category = "Trace|Spawns")
	int32 StartsPerTeam = 5;

	// --- Lighting --------------------------------------------------------------------------------
	//
	// Exposure is pinned: Config/DefaultEngine.ini sets r.DefaultFeature.AutoExposure=False, so the
	// tonemapper sees scene radiance unscaled and every intensity below is an absolute number rather
	// than something auto-exposure will normalise away. Measure with -TraceAutoShot after changing
	// any of them; do not assume.

	/**
	 * Forward-shading priorities for the four-light rig (art bible §5.1). Forward shading,
	 * translucency, water and volumetric fog honour exactly ONE directional light; with four spawned
	 * and no priorities declared the engine warned on screen every frame and picked a winner by
	 * overall brightness. The KeyLight is THE light (the rig's sole shadow caster), the atmosphere
	 * sun exists only to feed the sky capture, and Fill/Bounce are rim/ambient TINT that must never
	 * win the single-light slot. Static members rather than table literals because two translation
	 * units apply them: BuildLighting's FLightSpec table (TraceArenaBuilder.cpp) on the procedural
	 * map, and AdoptBakedArena (TraceArenaBake.cpp), which pushes the same policy onto a baked
	 * level's serialised lights at load so a stale bake self-heals without a re-bake.
	 */
	static constexpr int32 ForwardPriorityKeyLight = 10;
	static constexpr int32 ForwardPriorityAtmosphereSun = 5;
	static constexpr int32 ForwardPriorityRimLight = 0;

	/**
	 * Chromatic fringe, shared by BuildPostProcess (which owns the history comment, next to the
	 * setting) and AdoptBakedArena (which pushes it over the value serialised into a baked level's
	 * post-process volume — ApplyFidelity rewrites only the COST settings, so an art-direction
	 * change made after a bake reaches the shipping map through the adopt push, not a re-bake).
	 * 0.2 -> 0.0 for the release overhaul: at distance the fringe compounded TSR into green/magenta
	 * edges on thin neon (art bible §5.3).
	 */
	static constexpr float ArenaSceneFringeIntensity = 0.f;

	/**
	 * Illuminance of the light that drives ASkyAtmosphere, in lux. Very low, and aimed from BELOW
	 * the horizon (see AtmosphereSunRotation), so the sky renders as deep twilight rather than the
	 * bright blue daylight this arena shipped with. Its only job is to give the real-time sky light
	 * something non-black to capture.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	float AtmosphereSunIntensity = 0.55f;

	/**
	 * Positive pitch means the light travels UPWARD, i.e. the sun sits below the horizon. That is
	 * what makes the atmosphere night instead of day. Keep the pitch small and positive; go negative
	 * and you get a daytime sky back.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	FRotator AtmosphereSunRotation = FRotator(6.f, -35.f, 0.f);

	/** Cool key light. The only shadow caster - everything else is fill. */
	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	float KeyLightIntensity = 2.6f;

	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	FRotator KeyLightRotation = FRotator(-48.f, -40.f, 0.f);

	/** Warm shadowless rim from the opposite azimuth, so unlit faces separate from the background. */
	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	float FillLightIntensity = 1.8f;

	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	FRotator FillLightRotation = FRotator(-24.f, 145.f, 0.f);

	/**
	 * Faint cyan light travelling upward, standing in for the bounce off a floor covered in glowing
	 * lines. Emissive surfaces light nothing on their own here (Lumen is off, see DefaultEngine.ini),
	 * so without this the undersides of every character and structure are pure black.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	float BounceLightIntensity = 1.2f;

	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	FRotator BounceLightRotation = FRotator(58.f, 30.f, 0.f);

	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	float SkyLightIntensity = 0.7f;

	/**
	 * Candelas per floor lamp. There is a symmetric lattice of unshadowed point lights sitting a few
	 * hundred uu off the deck (see BuildFloorLamps), and they are the only thing in the rig that puts
	 * a GRAZING light on the vertical faces of cover blocks - the three directional lights all come
	 * from high angles, which is exactly why a 900 uu slab used to render as a featureless black
	 * shape you only noticed by walking into it.
	 *
	 * Units are candelas and the falloff is inverse-square, so the number is deceptive: at 8 m this
	 * is Intensity/64 lux, i.e. a lamp at 220 cd contributes about 3.4 lux at range - a little over
	 * KeyLightIntensity - while still making a bright pool directly beneath itself. Push it much past
	 * 350 and the pools merge into general ambient, which is the grey-box failure the palette comment
	 * warns about; that failure belongs to this dial and to the structure emissive, and of the two
	 * this is the one worth spending, because it arrives with a direction and therefore with shading.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	float FloorLampIntensity = 220.f;

	/**
	 * Attenuation radius of each floor lamp. Wide and dim beats narrow and hot for face reading.
	 *
	 * The lattice spacing is ~4200 uu, so a radius of 4200 is what makes neighbouring pools just
	 * touch. Below that the flanks go back to being lit only by the three directional lights, which
	 * is where this all started.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	float FloorLampRadius = 4200.f;

	/**
	 * Bloom strength on the unbound post-process volume. This is what makes emissive read as neon.
	 *
	 * Trimmed 2.4 -> 1.9 during integration. Two things landed at once that each add bloom energy:
	 * the arena gained face trim, ribs, buttresses and flank rails (692 -> 832 components, i.e. a lot
	 * more emissive surface per frame), and the camera moved to FIRST PERSON, which puts the player's
	 * eye much closer to that trim than the old 450 uu third-person arm ever did. Bloom is
	 * spread energy: measured against the same frame, disabling it entirely was worth +127% detail,
	 * so at 2.4 it was the single largest remaining source of the softness the user complained about.
	 * 1.9 keeps the glow tube (still well above the ~1.0 where emissive starts reading as a flat
	 * coloured rectangle) while cutting the full-frame haze, and it also takes the worst of the
	 * "face against a pylon strip blows the frame white" case with it. Do not take this below ~1.5:
	 * past that the neon stops being neon and the whole art direction goes with it.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|PostProcess")
	float BloomIntensity = 1.9f;

	/**
	 * Bloom threshold. Negative means "bloom everything, weighted by brightness" rather than only
	 * pixels above 1.0, which is what lets the dimmer floor grid glow instead of only the hot trim.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|PostProcess")
	float BloomThreshold = -0.35f;

	/**
	 * Height fog density. Low: enough to give the 33600 uu field depth, not enough to hide it.
	 *
	 * 0.012 -> 0.015. MEASURED over 52 walking frames: 3.8% of them were more than 85% DEAD BLACK -
	 * literal (0,0,0) - which is the other half of the point-blank defect and the one nobody
	 * photographs, because a black frame looks like a screenshot that failed. Those frames are all
	 * the same thing: a view direction with no structure in it, in a world whose ambient is a
	 * deliberate near-zero. Fog is the right instrument for exactly that - it is the only term that
	 * grows with DISTANCE, so it lifts an empty view off the floor of the tonemapper while adding
	 * nothing to the near field the contrast depends on. Kept small: this is also the dial that
	 * makes a scene look hazy, and haze is a complaint this project has already answered once.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|PostProcess")
	float FogDensity = 0.015f;

protected:
	/** Attach parent for every piece of built geometry. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Arena")
	TObjectPtr<USceneComponent> Root;

	// --- Build steps -----------------------------------------------------------------------------
	void BuildArena();
	void BuildFloorAndWalls(bool bBuildVisuals);
	void BuildGrid();
	void BuildCentreDais(bool bBuildVisuals);

	/**
	 * The four corner banks - the sketch's green arrows, i.e. the shallow stadium bowl.
	 *
	 * REPLACED the four stepped wing platforms and the two rows of segmented lane rails. Those
	 * existed to break up a 12000 uu wide field; at 9600 the banks do that job and do it as
	 * TERRAIN, which is what the sketch actually draws.
	 *
	 * Since spec v10 section 9 it emits TWO things per quadrant: nested box colliders on the coarse
	 * cove profile, and nested instanced shells on the fine one. See the wall-fillet block above.
	 */
	void BuildCornerBanks(bool bBuildVisuals);

	/**
	 * The concave cove along the base of every perimeter wall (spec v9 section 10).
	 *
	 * Four runs of nested boxes rising from Z = 0, exactly like a corner bank, so the union with the
	 * bank, with the floor and with the mode-B approach ramp is provably still walkable: each of
	 * those is a monotone staircase whose risers are under StepRise, and the pointwise MAXIMUM of
	 * two such staircases is another one (max is 1-Lipschitz). That is why this can be overlaid on
	 * the arena without auditing every surface it crosses. Spec v10 section 9 HALVED those risers,
	 * which cannot break the argument - a smaller bound is still a bound.
	 *
	 * Built inside bBuildCornerBanks' sibling switch bBuildWallFillets, and OUTSIDE the bBuildVisuals
	 * gate for the collision, because a dedicated server has to build the ground its clients are
	 * predicting against.
	 */
	void BuildWallFillets(bool bBuildVisuals);

	void BuildCoverField(bool bBuildVisuals);
	void BuildFlanks(bool bBuildVisuals);

	/**
	 * PATCH 28 §5 — THE SURF RAILS. Four of them, one per quadrant, in the outer lane between the two
	 * lane pylons. Read the block comment on the definition before moving one: the rail's face angles
	 * are DERIVED from the movement component's walkable limit, not typed beside it.
	 */
	void BuildSurfRails(bool bBuildVisuals);

	/**
	 * THE SIDE-WALL SURF BANDS. One continuous ride surface along each side wall, sitting on top of the
	 * corner bank's own cove and topping out on a bench at the buttress line, with a mitred plan fan
	 * and a sinking nose at each end. See the block comment on the definition; read bBuildSurfBanks
	 * before changing any of the numbers below, because all three of them are derived rather than set.
	 */
	void BuildSurfBanks(bool bBuildVisuals);

	/** Shallowest facet slope on a surf rail, degrees. Derived from the pawn's walkable limit. */
	float SurfRailMinFaceAngleDegrees() const;

	/** Crest height of a surf rail above the floor, uu. */
	float SurfRailHeight() const;

	/** Radius of the circular arc the rail's face is cut from, uu. */
	float SurfRailFaceRadius() const;

	/** How far the face reaches outboard from its toe, uu. */
	float SurfRailFaceSpan() const;

	/**
	 * How many planar facets the face's arc is cut into, for COLLISION and for the DRAWN shell.
	 *
	 * Both are DERIVED from a maximum allowed crease (SurfRailCollisionCreaseDegrees /
	 * SurfRailVisualCreaseDegrees) rather than typed, so retuning the surf band re-samples the arc
	 * instead of silently changing how coarse it is. The visual count is a MULTIPLE of the collision
	 * count, which makes every collision joint a drawn joint too — the property that keeps the
	 * collision chords from ever crossing the drawn ones.
	 */
	int32 SurfRailCollisionFacets() const;
	int32 SurfRailVisualFacets() const;

	/**
	 * How many equal-height bands the solid fill under the face is built from; the fill uses one box
	 * per band above the first. Derived from SurfRailFillBandRiseFactor, so it is independent of the
	 * facet count — see that constant for the seal argument.
	 */
	int32 SurfRailFillBands() const;

	/**
	 * THE JUNCTION BETWEEN THE LEVEL MAIN RUN AND THE SWEPT NOSE: TWO PLACEMENTS FROM ONE REACH.
	 *
	 * A swept facet's box is cut square to its own sweep, so its near face leans BACK past the
	 * section's start below the strip's mid-chord and FORWARD by the same amount above it.
	 * SurfRailNoseJointReach() is that amount, in |X|; the build laps the whole nose that far back
	 * along the sweep (which closes the forward half — twenty see-through wedge notches, one per drawn
	 * plate, before this) and sinks it by SurfRailNoseZOffset() = -Sink * 2 * reach (which keeps the
	 * backward half under the level run instead of proud of it).
	 *
	 * They are HERE rather than inside BuildSurfRails because Trace.Arena.SurfProfile needs them: an
	 * instrument that samples coarser than the feature it is asked about can only ever report that the
	 * feature is not there. The full geometric argument is in BuildSurfRails, above the call.
	 *
	 * -TraceSurfRailNoJunctionLap (Trace.Arena.SurfRailJunctionLap 0) restores the first swept nose's
	 * single-sign cancellation, notches and all, so the fix has a BEFORE arm inside the same binary
	 * and the drawn-shell probe's verdict can be falsified rather than only agreed with.
	 *
	 * THE OFFSET MOVES TWO EDGES, NOT ONE, and the second one cost two passes to notice. It lowers the
	 * whole cross-section of the FACE, so it lowers the top of the face relative to the CREST WEDGE
	 * over the nose — a ledge |ZOffset| uu tall running the nose's whole length, invisible to every
	 * probe in this file until Trace.Arena.SurfProfile §3c, because everything the rig fired ran ALONG
	 * travel and this feature runs ACROSS it. BuildSurfRails §4 sinks the crest wedge by the same
	 * offset so the two stay flush, and -TraceSurfRailNoCrestSink (Trace.Arena.SurfRailCrestSink 0)
	 * puts the ledge back so §3c has something to find.
	 */
	float SurfRailNoseJointReach() const;
	float SurfRailNoseStartLap() const;
	float SurfRailNoseZOffset() const;

	/** |Y| of the rail's inboard toe, uu. */
	float SurfRailToeY() const;

	/** |Y| of the rail's outboard back face, uu. */
	float SurfRailBackY() const;

	/** |X| of the rail's two ends: [0] nearest the halfway line, [1] nearest the goal. */
	void SurfRailRunX(float& OutNearX, float& OutFarX) const;

	/**
	 * DEMO 29 ITEM 4(a). |X| of the furthest a surfer's CAPSULE CENTRE can travel down the exit lane
	 * before something stops it — the first approach block or lane pylon whose Y band overlaps the
	 * rail's own, or the goal line if there is none. Swept off the same specs the build places, so a
	 * layout change moves it.
	 *
	 * IT IS NOT THE BlockAll FACE, and it returned that for three passes. Cover carries a pawn-only
	 * standoff shell NeonStandoff uu proud of its collision box and the pawn is PawnCapsuleRadius
	 * wide, so a centre stops 60 uu short of the box: 11740, not 11800, on the shipped field — which
	 * is where fifteen of fifteen instrumented rides actually stopped. BuildSurfRails re-measures it
	 * with a capsule swept down the real lane and errors if the two disagree.
	 */
	float SurfRailExitObstacleX() const;

	/**
	 * DEMO 29 ITEM 4(a). How much clear lane a rail's exit needs, in uu of |X| past the junction.
	 *
	 * Maximised along the nose (leaving later means leaving lower AND further out, and the worst case
	 * is not always at either end) using UTraceCharacterMovementComponent::GetSurfExitReach(), so it
	 * tracks the movement tuning instead of carrying a copy of it.
	 */
	float SurfRailExitClearance() const;
	void BuildEndzones(bool bBuildVisuals);

	/**
	 * The MODE B goals (spec v6 §4.3, rehung by spec v28 §8): a circular neon ring floating on each
	 * goal line, an approach ramp on each of its faces, the floor lane that leads to it from both
	 * directions and the scoring volume straddling its mouth. Built unconditionally and then hidden
	 * if mode A is the one armed - see the two-modes note in the class comment.
	 */
	void BuildGoals(bool bBuildVisuals);

	/**
	 * One end's worth of mode-B goal: the annulus that closes a square hole in nothing down to a
	 * circle, the neon hoop on each of its faces, and the two run-up ramps.
	 *
	 * WAS BuildGoalWall, AND THE RENAME IS THE CHANGE. Until spec v28 §8 this built a perforated
	 * REPLACEMENT for the end wall, which is why it had to agree with BuildFloorAndWalls about wall
	 * thickness, panels and standoffs, and why the solid slab it replaced was mode-tagged there. The
	 * wall is now 2400 uu further back and is nobody's business but its own: this builds a hoop
	 * hanging in the air and nothing else, and BuildFloorAndWalls builds one plain end wall for both
	 * modes.
	 */
	void BuildGoalRing(float Sign, bool bBuildVisuals);

	/**
	 * The sky dressing (release overhaul, MAP plan §4): a 24-tower silhouette ring 8,200–14,600 uu
	 * beyond the walls, a horizon glow band behind each end, and a 90 uu pillar of light over each
	 * goal ring. All of it is pooled ISM instances via AddMeshBlock, bCastShadow=false, no collision
	 * — pure wayfinding: the behind-goal skyline roofs, the bands and the beacons wear the defending
	 * team's colour (registered for the half-time repaint), so the sky itself answers "which way am
	 * I facing". The sky read needs visible sky, which the 2,600 uu wall takes away as you approach
	 * an end (the sightline arithmetic is stated where the band is built); the beacon, which stands
	 * INSIDE the arena over the ring, is the one that still answers from the pockets.
	 *
	 * Called inside the bBuildVisuals gate (a server has no sky) behind bBuildSkyline; @p
	 * bBuildVisuals is passed anyway so the signature matches its siblings if the gate ever moves.
	 */
	void BuildSkyline(bool bBuildVisuals);

	void BuildPlayerStarts();
	void BuildLighting();
	void BuildFloorLamps();
	void BuildPostProcess();

	/**
	 * Object flags for everything the builder creates - components, MIDs and spawned actors alike.
	 *
	 * RF_NoFlags at runtime (nothing is ever saved from a running game) and RF_Transient while an
	 * editor preview is being built, which is what stops the preview from being serialised into
	 * /Game/Maps/Arena and then doubling up against the arena the runtime builder makes for itself.
	 */
	EObjectFlags BuiltObjectFlags() const;

	/**
	 * Object flags for the ACTORS the build spawns - endzones, player starts, lights, fog, the
	 * post-process volume.
	 *
	 * RF_Transient normally, which is the rule that has always applied: these are runtime scaffolding
	 * and must never be serialised into a level, least of all by the editor preview button. RF_NoFlags
	 * during a bake, because a bake is the one time serialising them into a level is the entire point
	 * (spec v15 §1.4 - "the gameplay actors must exist in the saved level as ordinary placed actors").
	 */
	EObjectFlags SpawnedActorFlags() const;

	// --- BAKE RECORDING (spec v15 §1) --------------------------------------------------------------
	//
	// HOW THE BAKE REUSES THE BUILD instead of duplicating it. Spec v15 §1 is explicit that there
	// must not be a second builder, and it is right: a parallel "bake path" that laid out its own
	// arena would drift from this one in a week and the baked map would stop being the arena.
	//
	// So the bake runs THE SAME BuildArena(), as a transient editor preview, and simply WATCHES it:
	// the four primitive factories append a record here as well as doing their normal work. When the
	// build finishes, EmitBakedActors() groups those records back into logical pieces, spawns one
	// actor per piece, and then the preview is torn down as usual.
	//
	// WATCHING RATHER THAN DIVERTING is the whole reason this is safe, and it was not the first
	// design. A bake that RETURNED EARLY from the factories would have silently dropped every
	// customisation a caller applies to the component it gets back - and there is one that matters:
	// BuildCentreDais makes the Core pedestal's collision box ignore ECC_Pawn ("waist-high scenery
	// you can walk through beats an unwinnable match"). A diverted bake would have baked a
	// pawn-blocking pedestal in the dead centre of the field and nothing would have logged it.
	// Because the real component is built, the emit step copies its FINAL state, whatever a caller
	// did to it.

	/** One primitive the bake watched being built. Kinds mirror the four factories. */
	struct FTraceBakeRecord
	{
		enum class EKind : uint8
		{
			MeshBlock,      // AddMeshBlockRotated - visible geometry, an instance rather than a component
			CollisionBox,   // AddCollisionBlockRotated - the BlockAll boxes
			PawnStandoff,   // AddPawnStandoff - the pawn-only shells
			PointLight      // AddPointLight - the floor-lamp lattice
		};

		EKind Kind = EKind::MeshBlock;

		/** The DebugName the factory was called with. Becomes the actor label. */
		FName PieceName;

		/** MeshBlock only: builder-local transform, scale included (the shapes are 100 uu). */
		FTransform Transform = FTransform::Identity;

		/** MeshBlock only. Weak because the engine basic shapes outlive any bake. */
		TWeakObjectPtr<UStaticMesh> BlockMesh;

		/** MeshBlock only. Resolved to a committed MaterialInstanceConstant at emit time. */
		TWeakObjectPtr<UMaterialInstanceDynamic> BlockMID;

		/** MeshBlock only. A per-component flag on an ISM, so it is part of the pool key. */
		bool bCastShadow = false;

		/**
		 * Every other kind: the component the factory actually built, copied verbatim at emit time.
		 * That is what preserves per-call collision-response edits - see the note above.
		 */
		TWeakObjectPtr<USceneComponent> SourceComponent;

		/** Filled in by CollectPiecesSince, which is the only thing in this file that knows modes. */
		ETraceBakedScoringTag ModeTag = ETraceBakedScoringTag::Always;
	};

	/** Everything recorded by the current bake, in construction order. Empty outside a bake. */
	TArray<FTraceBakeRecord> BakeRecords;

	/**
	 * True only while BuildArena() is running for a bake.
	 *
	 * Declared unconditionally rather than under #if WITH_EDITOR for exactly the reason
	 * bBuildingEditorPreview is: BuiltObjectFlags() and the four primitive factories are the hottest
	 * paths in this file and must be one function with one body in every configuration.
	 */
	bool bBakingToLevel = false;

	/** Appends @p Record to BakeRecords. Returns false (and records nothing) outside a bake. */
	bool RecordForBake(const FTraceBakeRecord& Record);

	// --- ADOPTING A BAKED LEVEL --------------------------------------------------------------------

	/**
	 * Wires a level that was baked earlier back into the live state this builder owns, without
	 * constructing any geometry.
	 *
	 * WHY IT IS NEEDED AT ALL. The arena is not just meshes; it is four things this class holds
	 * pointers to and drives at runtime: the scoring volumes ApplyScoringMode arms, the mode-tagged
	 * furniture it shows and hides, the MIDs ApplyTeamSides repaints at half time, and the lights and
	 * post-process volume ApplyFidelity re-tunes on a quality change. A baked level has all of those
	 * as placed actors and none of them registered. Skipping the build WITHOUT adopting would give a
	 * level that looks right and then, at the first half-time switch, keeps both endzones painted the
	 * colour they were baked in and leaves both scoring shapes armed at once.
	 */
	void AdoptBakedArena();

	/**
	 * Destroys everything BuildArena() made - every component attached under Root, every actor in
	 * SpawnedActors, every MID, every instance pool - and resets the built flag so the next build
	 * starts from nothing.
	 *
	 * It used to be editor-only. It is not any more, because spec v7 §8's A/B rebuilds the arena in
	 * place between arms (RebuildForMeasurement) - but the RULE it encoded still holds: the GAME never
	 * calls this. At runtime the arena is built exactly once per world and EndPlay is the only teardown
	 * path the match itself ever takes.
	 */
	void DestroyBuiltArena();

	// --- Primitive helpers -----------------------------------------------------------------------

	/**
	 * Adds a visual-only mesh block. @p Size is the desired world size in uu; the engine basic
	 * shapes are 100 uu and centred on their pivot, so the scale is just Size/100.
	 * Silently does nothing if the mesh asset failed to resolve.
	 *
	 * SINCE THE INSTANCING PASS THIS DOES NOT PRODUCE A COMPONENT (spec v7 §8). The block becomes one
	 * INSTANCE inside a pooled UInstancedStaticMeshComponent shared by every other block with the same
	 * mesh, the same material and the same shadow flag - see AddInstancedBlock. That is why it now
	 * returns void: a caller that kept the old UStaticMeshComponent* would have been handed the whole
	 * batch, and anything it set (visibility, collision, a material) would have applied to hundreds of
	 * unrelated blocks. Nothing in this file ever used the return value.
	 */
	void AddMeshBlock(UStaticMesh* Mesh, const FVector& LocalCenter, const FVector& Size,
		UMaterialInstanceDynamic* MID, bool bCastShadow, const TCHAR* DebugName, float YawDegrees = 0.f);

	/**
	 * Adds an invisible box collider. Collision deliberately lives in box components rather than in
	 * the static meshes: it costs less, and it means the floor and walls still exist even if
	 * /Engine/BasicShapes ever fails to resolve (a dedicated server build, for instance) - players
	 * would see nothing but would not fall out of the world.
	 */
	UBoxComponent* AddCollisionBlock(const FVector& LocalCenter, const FVector& Size, const TCHAR* DebugName,
		float YawDegrees = 0.f);

	// --- Freely rotated variants (spec v6 §4.3) ---------------------------------------------------
	//
	// The three helpers above take a YAW only, which was enough while every piece of the arena was an
	// axis-aligned box standing on the floor. A ring set into a VERTICAL wall is not: its segments
	// are spokes around an axis that points down the field, so each one needs a ROLL. Rather than
	// widening five signatures (and their forty-odd call sites) these are separate entry points, and
	// the yaw-only versions forward to them, so there is still exactly one implementation of each.

	/** As AddMeshBlock, with a full rotation rather than a yaw. */
	void AddMeshBlockRotated(UStaticMesh* Mesh, const FVector& LocalCenter, const FVector& Size,
		UMaterialInstanceDynamic* MID, bool bCastShadow, const TCHAR* DebugName, const FRotator& Rotation);

	/** As AddCollisionBlock, with a full rotation rather than a yaw. */
	UBoxComponent* AddCollisionBlockRotated(const FVector& LocalCenter, const FVector& Size, const TCHAR* DebugName,
		const FRotator& Rotation);

	// --- INSTANCING (spec v7 §8) ------------------------------------------------------------------
	//
	// THE PROBLEM THIS SOLVES, with the measured numbers. Every visible block used to be its own
	// UStaticMeshComponent: 1454-1462 registered primitives, all Movable, submitted as ~1450 separate
	// draws per pass and again per shadow cascade, on an arena that is a handful of engine cubes and
	// one cylinder repeated hundreds of times. Game thread was 1.8-3.1 ms, so none of that was CPU
	// simulation - it was draw submission and GPU.
	//
	// THE FIX. Blocks are pooled by (mesh, material, cast-shadow) and become instances of one
	// UInstancedStaticMeshComponent per pool. The arena's look is UNCHANGED - the same cubes at the
	// same transforms with the same materials - only how they reach the renderer changes.
	//
	// WHY THE KEY IS EXACTLY THOSE THREE. Mesh and material are what a draw call is; two blocks that
	// differ in either cannot share one. bCastShadow is a per-COMPONENT flag on an ISM (there is no
	// per-instance shadow bit), so a shadow-caster and a non-caster cannot share a pool either -
	// merging them would silently start casting shadows off every neon strip in the arena.
	//
	// WHY THE MIDs DO NOT EXPLODE THE POOL COUNT: MakeSurfaceMID/MakeNeonMID are called once per build
	// STEP and the result is handed to every block in it (one body MID and two neon MIDs for the whole
	// cover field, one per half), so ~1450 blocks collapse into a few dozen pools. Half-time repainting
	// still works untouched, because it pushes parameters into those same MIDs (ApplyTeamSides).

	/**
	 * The pooled ISM that carries every block sharing one (mesh, material, shadow) key.
	 *
	 * Registration is DEFERRED to FlushInstancePools(): instances are appended while the component is
	 * still unregistered, so the whole batch costs one render-state creation instead of one per block,
	 * and the mobility can be decided after the build knows whether anything in the pool has to move.
	 */
	struct FTraceInstancePool
	{
		TWeakObjectPtr<UStaticMesh> Mesh;
		TWeakObjectPtr<UMaterialInstanceDynamic> MID;
		bool bCastShadow = false;

		TWeakObjectPtr<UInstancedStaticMeshComponent> Component;

		/**
		 * Set by CollectPiecesSince when a mode-tagged instance lands in this pool.
		 *
		 * Such a pool must stay Movable: arming a scoring mode rewrites those instances' transforms,
		 * and rewriting a Static component's transform in a game world is exactly what Static means
		 * you may not do. Every other pool is genuinely Static - see FlushInstancePools.
		 */
		bool bNeedsRuntimeInstanceUpdates = false;
	};

	/**
	 * One block that was turned into an instance, remembered in construction order.
	 *
	 * This is the instanced half of the mark/collect diff: the pooled ISMs interleave blocks from
	 * every build step, so "the instances added since the mark" cannot be read off any one component
	 * and has to be recorded here as it happens.
	 */
	struct FTraceBuiltInstance
	{
		int32 PoolIndex = INDEX_NONE;
		int32 InstanceIndex = INDEX_NONE;
		FTransform Transform;
	};

	/** Appends one instance to the pool for (@p Mesh, @p MID, @p bCastShadow), creating it if needed. */
	void AddInstancedBlock(UStaticMesh* Mesh, UMaterialInstanceDynamic* MID, bool bCastShadow,
		const FTransform& LocalTransform, const TCHAR* DebugName);

	/**
	 * Registers every pool that has instances, at the mobility the build decided it needs.
	 *
	 * Called once at the end of BuildArena, AFTER every CollectPiecesSince, because that is when
	 * bNeedsRuntimeInstanceUpdates is finally known - and a component's mobility can only be chosen
	 * before it registers.
	 */
	void FlushInstancePools();

	/**
	 * An invisible box that blocks ECC_Pawn AND NOTHING ELSE, wrapped around a piece of structure so
	 * a first-person eye cannot be pressed onto the unlit neon that decorates it.
	 *
	 * This is the point-blank whiteout fix, and the "and nothing else" is the whole of it. Every
	 * neon strip in this file used to be built PROUD of the collision box it decorates - by 10 to 36
	 * uu - while the pawn capsule (radius 34) stops at the collision box, so the eye ended up closer
	 * to the emissive than to the surface it was drawn on, and in the worst case (a wall rib) INSIDE
	 * it. Unlit emissive is distance-invariant, so a strip a few uu from the lens arrives at full
	 * intensity across the whole frame: 5% of walking frames measured over 40% blown out, one 99%.
	 *
	 * Growing the REAL collision box would have fixed it and moved every sightline, bullet, peek
	 * angle and camera probe outward with it. This shell moves only where a body may stand:
	 * ECC_Visibility, ECC_Camera and every other channel stay Ignore, so shooting, bot line of sight
	 * and the third-person spring arm behave exactly as they did before.
	 *
	 * It is built on dedicated servers too - it is collision the clients predict against.
	 *
	 * @param Size  outer size of the shell, i.e. the structure's size already inflated by
	 *              TraceArenaConstants::NeonStandoff (or WallNeonStandoff). Never inflated in Z:
	 *              the top face of a block is a surface the game reasons about.
	 */
	UBoxComponent* AddPawnStandoff(const FVector& LocalCenter, const FVector& Size, const TCHAR* DebugName,
		float YawDegrees = 0.f);

	/**
	 * The workhorse for every piece of interior structure: one dark lit body, one blocking box, a
	 * thin unlit neon "lip" wrapped just under its top face, and - for anything tall enough to stand
	 * behind - a full set of FACE TRIM: a skirt around the base, a band around the middle, and eight
	 * vertical ribs (one on each corner, one down the centre of each face).
	 *
	 * WHY THE FACE TRIM EXISTS. The first pass had the lip and nothing else, and it was measured to
	 * fail in the worst possible way: the lip reads beautifully from above and from a distance, but a
	 * cover block seen HEAD ON presents only its vertical face, which is near-black albedo lit by
	 * three high-angle directional lights - i.e. nothing. One screenshot was a player pressed into a
	 * featureless black slab filling a third of the frame. Cover you cannot see is not cover, so
	 * every vertical face now carries its own light: two horizontal lines that give it scale and four
	 * vertical ribs that give it a silhouette. All of it is visual-only, so none of it can interfere
	 * with stepping onto the block it decorates.
	 *
	 * @param bCollide      false for pure scenery. True gives it a BlockAll box, which also blocks
	 *                      hitscan (ECC_Visibility) and therefore breaks bot line of sight - that is
	 *                      deliberate.
	 * @param FaceNeonMID   material for the face trim. Null reuses @p NeonMID; callers pass a dimmer
	 *                      instance so the top lip stays the brightest line on the shape.
	 * @param bVerticalTrim false suppresses the corner ribs for pieces where they read as clutter -
	 *                      the nested tiers of a stepped platform and the horizontal gate beam.
	 * @param bFaceBands    false ALSO suppresses the skirt and the mid band, leaving the top lip as
	 *                      the only trim. That is what a terrace of the corner banks wants: the
	 *                      terraces are nested, so a skirt on an inner terrace would be buried
	 *                      inside the solid body of the one outside it, and a mid band would be a
	 *                      second line 20 uu under the lip. One glowing contour per terrace reads as
	 *                      a bank; three read as a pile of crates.
	 */
	void AddNeonBlock(const FVector& LocalCenter, const FVector& Size, float YawDegrees,
		UMaterialInstanceDynamic* BodyMID, UMaterialInstanceDynamic* NeonMID,
		bool bCollide, const TCHAR* DebugName,
		UMaterialInstanceDynamic* FaceNeonMID = nullptr, bool bVerticalTrim = true,
		bool bFaceBands = true);

	/**
	 * A pilaster standing flush against a perimeter wall: a dark body, a blocking box and one
	 * full-height neon strip on the face that looks back into the field.
	 *
	 * Deliberately much cheaper than AddPylon (three components rather than six) because there are
	 * thirty of them. They stick only ~200 uu proud of the wall, so the shallow alcoves between them
	 * cannot trap a bot steering directly at a target.
	 *
	 * @param InwardAxis unit vector pointing from the wall back into the field.
	 */
	void AddWallButtress(const FVector2D& LocalCentre, const FVector2D& InwardAxis, float Width, float Depth,
		float Height, UMaterialInstanceDynamic* BodyMID, UMaterialInstanceDynamic* NeonMID, const TCHAR* DebugName);

	/** Unshadowed local light. Used for the floor-lamp lattice; see FloorLampIntensity. */
	UPointLightComponent* AddPointLight(const FVector& LocalCenter, float AttenuationRadius, float Intensity,
		const FLinearColor& Color, const TCHAR* DebugName);

	/**
	 * A stack of concentric blocks forming a walkable stepped platform (a ziggurat).
	 *
	 * Tier N spans the floor up to its own top, so the tiers are nested rather than balanced on each
	 * other, and every riser is StepRise tall. StepRise MUST stay under UCharacterMovementComponent's
	 * MaxStepHeight (engine default 45 - this project does not override it) or the platform becomes a
	 * wall. Because it steps up identically from all four sides there is no ramp to find and no
	 * concave pocket to get trapped in, which matters: Trace's bots steer directly at their target
	 * with no navmesh (see ATraceBotController's header).
	 */
	void AddSteppedPlatform(const FVector2D& LocalCentre, float TopTierSide, float TierSideStep,
		int32 TierCount, float YawDegrees, UMaterialInstanceDynamic* BodyMID,
		UMaterialInstanceDynamic* NeonMID, const TCHAR* DebugName);

	/** Vertical light column: a dark shaft with a full-height neon strip on each of its four faces. */
	void AddPylon(const FVector2D& LocalCentre, float Side, float Height,
		UMaterialInstanceDynamic* BodyMID, UMaterialInstanceDynamic* NeonMID, const TCHAR* DebugName);

	/**
	 * Resolves SurfaceMaterial/NeonMaterial and logs which of the three arms answered.
	 *
	 * Committed parents (constructor FObjectFinder) -> legacy /Game/Generated (runtime LoadObject,
	 * deliberately NOT a finder so the cooker never records a gitignored dependency) -> the engine
	 * grey. Spec v17 §0 rule 1. Idempotent; safe to call from any build or adopt path.
	 */
	void ResolveArenaMaterials();

	/**
	 * Dark lit structural surface. All parameters are M_TraceSurface's; see generate_content.py.
	 *
	 * @param ParentOverride an M_TraceSurface-derived MIC to parent the MID to instead of the bare
	 *                       SurfaceMaterial — how the floor picks up MI_Surface_Floor_Grid's armed
	 *                       static switch. Null (every other call site) means SurfaceMaterial.
	 */
	UMaterialInstanceDynamic* MakeSurfaceMID(const FLinearColor& BaseColor, float Roughness,
		float Metallic = 0.f, const FLinearColor& Emissive = FLinearColor::Black, float EmissiveStrength = 1.f,
		UMaterialInterface* ParentOverride = nullptr);

	/** Unlit neon. @p Glow multiplies @p Color; > 1 is what pushes it over the bloom threshold. */
	UMaterialInstanceDynamic* MakeNeonMID(const FLinearColor& Color, float Glow);

	/**
	 * As above, plus M_TraceNeon's uniform pulse: the emissive term is multiplied by
	 * 1 + PulseAmp * sin(2π * Time * PulseRate) — entirely CPU-folded uniform expressions, zero
	 * per-pixel cost (see the pulse chain in generate_content.py). BOTH parameters must be non-zero
	 * for any motion; the parents default both to 0, which is how the must-not-pulse list (trails,
	 * parry, territory trim, cover lips, every T0/T1 surface) is enforced without a lock.
	 *
	 * @param PulseRate cycles per second (Hz).
	 * @param PulseAmp  swing as a fraction of Glow (0.12 = ±12%).
	 */
	UMaterialInstanceDynamic* MakeNeonMID(const FLinearColor& Color, float Glow, float PulseRate, float PulseAmp);

	// --- Derived layout --------------------------------------------------------------------------
	float HalfLength() const { return FieldLength * 0.5f; }
	float HalfWidth() const { return FieldWidth * 0.5f; }

	// PlayerHeightUU() is declared PUBLIC, with the rest of the arena's oracle surface. It used to
	// live here, among the protected derived-layout helpers; spec v28 §8's goal-side harness needs it
	// and so does anything else that wants to know how big a person is.

	/**
	 * |Y| at which the corner banks start, i.e. the half width of the FLAT central playfield.
	 *
	 * Everything laid on the floor (the grid, the lane stripes) and every piece of cover has to
	 * stay inside this or it would be built partly inside a bank - a floor decal buried under a
	 * terrace, or a 176 uu block with its bottom step swallowed. Returns HalfWidth() outright when
	 * the banks are switched off, so the same call site is correct either way.
	 */
	float BankInnerHalfWidth() const;

	/**
	 * How far the wall cove's TOE reaches in from a wall face, or 0 when the cove is switched off.
	 *
	 * Anything drawn flat on the floor within this of a wall would be buried inside the cove, so the
	 * two endzone sideline rails measure themselves against this rather than against the sideline.
	 */
	float WallFilletToeDepth() const;

	/** Top of the centre diamond, i.e. the surface the Core pedestal stands on. */
	float DaisTopZ() const;

	/**
	 * |X| of a goal line, i.e. HalfLength() minus the endzone depth. Both ends are symmetric.
	 *
	 * The anchor for everything at the business end of the field: the endzone gate, the goal posts,
	 * the mode-B mouth, and the goal-approach cover cluster (see ApproachCover in the .cpp), which is
	 * measured back from HERE rather than as a fraction of the half length. That distinction is what
	 * stopped the 3.5:1 lengthening from sliding the goal-approach tower into the spawn fan - the
	 * pads are goal-relative, so cover that is length-relative drifts THROUGH them as the field grows.
	 */
	float GoalLineX() const;

	// --- Mode-tagged geometry ---------------------------------------------------------------------
	//
	// Both scoring shapes are built; one is presented. These three are the whole mechanism.

	/**
	 * One piece of geometry that belongs to a single scoring mode, plus the state it was built with,
	 * so hiding and re-showing it restores exactly what it had rather than a guess.
	 *
	 * TWO KINDS OF PIECE SINCE THE INSTANCING PASS. Collision boxes and pawn shells are still whole
	 * components and are hidden by SetVisibility/SetCollisionEnabled. Visible geometry is no longer a
	 * component at all - it is one instance inside a pooled ISM - so it is hidden by collapsing that
	 * instance's transform to zero scale and restored by writing the built transform back. Exactly
	 * one of the two halves is filled in; InstanceIndex == INDEX_NONE says which.
	 */
	struct FTraceModePiece
	{
		TWeakObjectPtr<USceneComponent> Component;

		/** Collision the component was built with. Restored verbatim when its mode is armed. */
		TEnumAsByte<ECollisionEnabled::Type> Collision = ECollisionEnabled::NoCollision;

		/** Instanced half: the pool the piece lives in, or null for a whole-component piece. */
		TWeakObjectPtr<UInstancedStaticMeshComponent> InstancePool;

		/** Index of this piece's instance inside InstancePool, or INDEX_NONE. */
		int32 InstanceIndex = INDEX_NONE;

		/** The transform the instance was built with. Written back when its mode is armed. */
		FTransform InstanceTransform = FTransform::Identity;
	};

	/**
	 * A point in the build, against which "everything made since" can be diffed.
	 *
	 * Two counters because the build now emits two kinds of thing: whole components attached under
	 * Root (collision boxes, pawn shells, lights) and INSTANCES appended to the pooled ISMs. Both
	 * append in construction order, so a pair of counts is a complete cut of the build.
	 */
	struct FTraceBuildMark
	{
		int32 Components = 0;
		int32 Instances = 0;

		/**
		 * Third counter for the bake (spec v15 §1). A bake builds no components and no instances - it
		 * records - so without this a mode-tagged build step would collect an empty range and the
		 * baked level would present mode A's endzone paint and mode B's ring at the same time.
		 */
		int32 BakeRecords = 0;
	};

	/**
	 * Where the build has got to right now.
	 *
	 * Paired with CollectPiecesSince() this is how a build step tags everything it made without any
	 * of the primitive helpers knowing modes exist: mark, build, collect the difference. Every helper
	 * in this file either attaches to Root or appends to BuiltInstances, and both append in order, so
	 * the difference IS what the step built.
	 */
	FTraceBuildMark MarkBuiltComponents() const;

	/** Appends everything built since @p Mark to @p Out, with the state it was built with. */
	void CollectPiecesSince(const FTraceBuildMark& Mark, TArray<FTraceModePiece>& Out);

	/** Shows or hides one tagged set, restoring its built collision when shown. */
	static void SetPiecesPresented(const TArray<FTraceModePiece>& Pieces, bool bPresented);

	/**
	 * +1 for the team defending the +X end, -1 for the other, AS THE ARENA IS PAINTED AT BUILD TIME.
	 *
	 * NOT the runtime authority any more. Spec §1 switches sides at half time, so "which end does
	 * Blue defend" is a replicated fact that changes: ask ATraceGameState::GetDefendedEndSign().
	 * This function now means only "which end did the builder originally paint in this colour",
	 * which is exactly what ApplyTeamSides() needs in order to repaint it.
	 */
	static float TeamEndSign(ETraceTeam Team);

public:
	/**
	 * Repaints every team-coloured arena surface for a new side assignment (spec §1 half-time
	 * switch): endzones, goals, gates, end walls, floor grid halves, cover lips and face trim,
	 * bank contours, flank dressing, corner pylons, skyline roofs, horizon bands, goal beacons —
	 * and the floor-lamp lattice, which is lights rather than materials and is re-blended directly.
	 *
	 * Without this, a Blue player spends the second half standing in an orange-painted endzone
	 * defending it — the arena is the single largest piece of "which way am I attacking" signage in
	 * the game, and a stale one actively misinforms.
	 *
	 * Every recolourable surface is registered at build time into SideMIDs, indexed by the X sign of
	 * the end it belongs to, so this is a colour push over a small array rather than a rebuild. It
	 * is idempotent and safe to call however many times, from any machine.
	 *
	 * @param TeamOnNegativeSide the team DEFENDING the -X end for the half about to be played.
	 */
	void ApplyTeamSides(ETraceTeam TeamOnNegativeSide);

	/**
	 * Finds this world's builder and repaints it. Called from BOTH sides of the network: the game
	 * mode drives it on the server, ATraceGameState::OnRep_SidesChanged drives it on clients. The
	 * builder is not replicated (each machine constructs its own arena), so there is no single
	 * authoritative call site that could cover both.
	 */
	static void ApplyTeamSidesInWorld(const UWorld* World, ETraceTeam TeamOnNegativeSide);

#if !UE_BUILD_SHIPPING
	/**
	 * DEV LEVER for Trace.Arena.PulseTest: force PulseRate/PulseAmp onto every material instance the
	 * builder made, and report how many took the parameters.
	 *
	 * It exists because a still photograph cannot prove a 0.25 Hz breath: the shipped amplitudes are
	 * ±12% of an emissive value that is already several times over the tonemapper's white point, so
	 * two frames half a period apart differ by a fraction of a display code value even when the
	 * material is animating perfectly. Driving the same parameters to an unmissable rate/amplitude
	 * for one capture run turns "is the pulse chain alive at all?" into a question a screenshot pair
	 * can answer — without a rebuild, and without shipping test values in the build.
	 *
	 * @return the number of MIDs that reported the parameters present (0 means the parent has no
	 *         pulse chain: the assignments in MakeNeonMID are landing on nothing).
	 */
	int32 ForcePulseOnAllMIDs(float PulseRate, float PulseAmp);

	/**
	 * MEASURES the surf rail in quadrant (@p XSign, @p YSign) by firing traces at it — see the
	 * implementation's block comment. Drives Trace.Arena.SurfProfile.
	 *
	 * It exists because every safety property BuildSurfRails() claims about the solid it makes ("the
	 * overlap can only add material BELOW the ridable surface", "a staircase that only steps down
	 * presents no face to a rider") is a property of the SHAPE, and this project has already shipped
	 * one surf nose whose correctness was argued rather than measured and which left ~10 uu lips.
	 *
	 * IT HAS BEEN WRONG THREE TIMES ITSELF, AND THE THREE WAYS IT WAS ARE WHY IT NOW LOOKS WHERE IT
	 * DOES.
	 *   1. It sampled along travel at 10 uu across three of the band's 447 uu, so it reported "worst
	 *      RISE +0.0 uu" about a junction feature 13.3 uu long that its samples straddled. Both
	 *      resolutions are now derived from SurfRailNoseJointReach() and from the drawn facet cut.
	 *   2. It traced only COLLISION, so a shell with twenty see-through holes in it read clean. §5
	 *      builds a height field for the DRAWN shell from the instance transforms.
	 *   3. EVERYTHING IT FIRED RAN ALONG TRAVEL. The lateral stations existed only to place an
	 *      along-travel finding, never to compare two of them with each other, so a discontinuity
	 *      that is constant along travel was invisible — and one pass created a 13.3 uu one along the
	 *      whole nose without being able to see it. §3c walks ACROSS travel, in the RESIDUAL against
	 *      the designed cross-section (raw Z is useless on a 61-degree face), and separates a step on
	 *      the ride surface from a step on the walkable crest.
	 * The pawn's own capsule is swept through the junction rather than argued about, and §6 sweeps it
	 * down the flight off the junction AT THE LATERAL DRIFT A RIDE CARRIES — the variable an earlier
	 * version pinned at zero, which is the one that decides the answer.
	 *
	 * Anything added here should be asked the same question first: could this probe tell the
	 * difference between "clean" and "too coarse to see"? And the follow-up this file learned the
	 * hard way: is there an AXIS it does not look along?
	 */
	void LogSurfRailProfile(float XSign, float YSign) const;
#endif

private:
	/**
	 * GetSideRampProbe's memo, one slot per (XSign, YSign) quadrant, indexed 0..3.
	 *
	 * mutable because the probe is const — it MEASURES the level rather than changing it, and a
	 * caller asking "where is the ridable side ramp" should not have to hold a non-const arena. Only
	 * a VALID probe is kept: see the function, which explains why a miss has to be retried.
	 */
	mutable FTraceSurfRailProbe SideRampProbeCache[4];

	/** World time before which a failed side-ramp measurement is not attempted again. */
	mutable double SideRampProbeRetryAfter[4] = { 0.0, 0.0, 0.0, 0.0 };

	/**
	 * One recolourable arena surface, remembered so half time can repaint it.
	 *
	 * The colour is not stored: it is re-derived from the team that currently defends this end, so
	 * the array cannot fall out of step with the side assignment.
	 */
	struct FTraceSideMID
	{
		/** Which end this surface belongs to: -1 or +1 along X. */
		float EndSign = 0.f;

		/**
		 * WEAK on purpose, and it is not a leak risk: every MID these come from is already held
		 * strongly by TintMIDs (a UPROPERTY), so this array only needs to observe. That also keeps
		 * FTraceSideMID a plain C++ struct rather than a USTRUCT, which it has no other reason to be.
		 */
		TWeakObjectPtr<UMaterialInstanceDynamic> MID;

		/** Neon MIDs take Color/Glow; surface MIDs take BaseColor/Emissive. Different pushes. */
		bool bNeon = false;

		/** Glow for a neon MID, or emissive strength for a surface MID. */
		float Intensity = 0.f;

		/** Surface MIDs only: how far the base colour is dimmed from the team colour. */
		float BaseDim = 0.f;
	};

	/** Registers @p MID as belonging to the end at @p EndSign. No-op on null. */
	void RegisterSideMID(float EndSign, UMaterialInstanceDynamic* MID, bool bNeon, float Intensity, float BaseDim = 0.f);

	/**
	 * Every arena surface whose colour follows the side assignment — the endzone/goal/gate/end-wall
	 * set that always registered, plus (release overhaul, MAP plan §5) the cover lips and face trim,
	 * the bank contours, the flank dressing (with the corner pylons and end buttresses that share
	 * its MIDs) and the new sky pieces. Built once, repainted at half time.
	 */
	TArray<FTraceSideMID> SideMIDs;

	/** The side assignment currently painted, so ApplyTeamSides can skip redundant work. */
	ETraceTeam PaintedTeamOnNegativeSide = ETraceTeam::Blue;

	/**
	 * The scoring shape currently presented. Defaults to mode A, which is the shipped game.
	 *
	 * Set before the build (from ATraceGameState, or from UTraceSettings if no game state exists yet)
	 * and re-applied by ApplyScoringMode whenever the authority says it changed.
	 */
	// ENUMERATOR NAMES: EndzoneStatusCore / ThrownCoreAndGoals, as declared in TraceSettings.h and as
	// serialised by Config/DefaultGame.ini. This line said ETraceScoringMode::Endzones, which does
	// not exist and failed the whole module; TraceIsGoalMode() is the readable way to test it.
	ETraceScoringMode ScoringMode = ETraceScoringMode::EndzoneStatusCore;

	/** Furniture that only exists in mode A: the full-width patch, goal line and endzone edge rails. */
	TArray<FTraceModePiece> EndzoneModePieces;

	/** Furniture that only exists in mode B: the goal posts, crossbar and lit mouth. */
	TArray<FTraceModePiece> GoalModePieces;

	/**
	 * The same two sets on a BAKED level, where a piece is a whole actor rather than a component.
	 *
	 * Kept separate from the two arrays above rather than folded into FTraceModePiece because hiding
	 * an actor and hiding a component are different calls (SetActorHiddenInGame / SetActorEnableCollision
	 * against SetVisibility / SetCollisionEnabled), and because exactly one of the two mechanisms is
	 * ever populated: a procedural arena has no baked actors and a baked one has no built components.
	 */
	TArray<TWeakObjectPtr<ATraceBakedPiece>> BakedEndzoneModeActors;
	TArray<TWeakObjectPtr<ATraceBakedPiece>> BakedGoalModeActors;

	/** Shows or hides one baked set. The actor-level twin of SetPiecesPresented. */
	static void SetBakedActorsPresented(const TArray<TWeakObjectPtr<ATraceBakedPiece>>& Actors, bool bPresented);

	/**
	 * Resolved answer for FindPlacedCoreSpawn(), so the level is not swept on every kickoff.
	 *
	 * MUTABLE because GetCoreSpawnLocation() is const and must stay const - ATraceCore asks it from a
	 * const accessor. Weak rather than strong so that deleting the marker in the editor re-resolves
	 * instead of returning a dangling answer, and not a UPROPERTY for the same reason the instance
	 * pools are not: this is a cache of something the world already owns.
	 */
	mutable TWeakObjectPtr<ATraceCoreSpawn> PlacedCoreSpawn;

	/**
	 * Every ATraceEndzone this builder spawned, both shapes, server only.
	 *
	 * Weak, and separate from SpawnedActors (which owns them): ApplyScoringMode has to arm one pair
	 * and disarm the other, and walking a typed list beats filtering the actor list by cast on every
	 * toggle. Also empty on clients, where no scoring volume is ever spawned.
	 */
	TArray<TWeakObjectPtr<ATraceEndzone>> ScoringVolumes;


	/**
	 * One ISM per (mesh, material, shadow) key. Rebuilt from nothing on every build.
	 *
	 * Weak handles rather than a UPROPERTY array: an actor component whose Outer is this actor is
	 * added to the actor's owned-component set the moment it is constructed, so it is already GC
	 * reachable - exactly as every UStaticMeshComponent this file has ever made was, none of which
	 * were held in a UPROPERTY either.
	 */
	TArray<FTraceInstancePool> InstancePools;

	/** Every instance added, in construction order. The instanced half of the mark/collect diff. */
	TArray<FTraceBuiltInstance> BuiltInstances;

	/** Engine basic shapes, resolved in the constructor so the cooker keeps them (contract §2). */
	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CylinderMesh;

	/** Fallback only: /Engine/BasicShapes/BasicShapeMaterial. Lit, no emissive input. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> BaseMaterial;

	/** /Game/Trace/Materials/Parents/M_TraceSurface - see the asset note in the class comment. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SurfaceMaterial;

	/** /Game/Trace/Materials/Parents/M_TraceNeon. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> NeonMaterial;

	/**
	 * /Game/Trace/Materials/Authored/MI_Surface_Floor_Grid — the hand-authored micro-grid floor
	 * instance (parent M_TraceSurface, bUseGrid armed; see Scripts/author_mics.py). The main floor
	 * MID is created with THIS as its parent, so the floor carries the faint cyan micro-grid on both
	 * maps: MIDs inherit a MIC parent's static switches, and the re-bake writes the baked floor MIC
	 * with the same parent. Absent (older content), the floor falls back to SurfaceMaterial and
	 * renders exactly as it did before the grid existed — see ResolveArenaMaterials.
	 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> FloorGridMaterial;

	/** Keeps the tint instances alive independently of the components they are assigned to. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> TintMIDs;

	/** Actors we spawned, so a destroyed builder takes its arena with it instead of orphaning it. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> SpawnedActors;

	/** BeginPlay can be forced early by the GameMode (DispatchBeginPlay); never build twice. */
	bool bArenaBuilt = false;

	/**
	 * Which geometry path this arena was built with (spec v7 §8). Latched at the top of BuildArena
	 * from Trace.Arena.Instancing and -TraceArenaLegacyGeometry, and never read again after the
	 * build, so the arena can never come out half instanced.
	 */
	bool bBuildingInstancedGeometry = true;

	/**
	 * THE BEFORE ARM FOR SPEC v9 SECTION 10, latched the same way and for the same reason.
	 *
	 * Spec v9 section 0 is a post-mortem on a fix that was reported green by a harness that never went
	 * red, so the square-cornered arena has to still be buildable FROM THIS BINARY: the corner banks
	 * fall back to evenly spaced terraces (a straight ramp meeting the wall at 90 degrees) and the
	 * wall cove is not built at all. Set with Trace.Arena.WallCove 0 before the arena is built, or -
	 * because the arena is built from ATraceGameMode::PreInitializeComponents, earlier than any
	 * console command can run - with -TraceArenaSquareCorners on the command line.
	 */
	bool bBuildingSquareCorners = false;

	/**
	 * True only while BuildArena() is running for the editor preview.
	 *
	 * Declared unconditionally rather than under #if WITH_EDITOR so that BuiltObjectFlags() - which
	 * every primitive helper calls - is one function with one body in every configuration. It is a
	 * bool; the alternative is a preprocessor fork through the hottest path in the file.
	 */
	bool bBuildingEditorPreview = false;

	/** True while an editor preview is standing. See the preview block in the public section. */
	bool bEditorPreviewBuilt = false;

	// --- Fidelity state (spec v11 §3). Everything ApplyFidelity() re-tunes, remembered at build time
	//     so re-tuning is a property write rather than a search of the world.

	/** The unbound post-process volume BuildPostProcess spawned. Weak: SpawnedActors owns it. */
	TWeakObjectPtr<APostProcessVolume> ArenaPostProcess;

	/**
	 * The ONE shadow-casting directional light (see the FLightSpec table in BuildLighting).
	 *
	 * Remembered rather than re-found because the Shadows row has to be able to switch the whole
	 * cascade pass off, and "the light that casts shadows" is a build-time fact, not something to
	 * rediscover by iterating ADirectionalLight and guessing which one it is.
	 */
	TWeakObjectPtr<UDirectionalLightComponent> KeyLightComponent;

	/**
	 * The floor-lamp lattice, in build order, so the Effects row can light a subset of it.
	 *
	 * These are unshadowed point lights, but on a per-pixel-bound frame an unshadowed light is still
	 * a full deferred lighting pass over its screen footprint, and at FloorLampRadius 4200 that
	 * footprint is large. Turning half of them off is one of the few Low-preset levers this file
	 * owns that costs no geometry - see ApplyFidelity.
	 *
	 * Also the half-time repaint's lamp list (MAP plan §5.4): lamps are tinted toward the half's
	 * defending team at build, and a light is not a MID, so ApplyTeamSides re-blends these directly.
	 * Both build paths fill the array — AddPointLight on the procedural map, AdoptBakedArena's
	 * FloorLamp-tagged collection on the baked one — so the repaint needs no third bookkeeping list.
	 */
	TArray<TWeakObjectPtr<UPointLightComponent>> FloorLamps;

	/** Intensity and radius each lamp was built with, so re-tuning is absolute rather than relative. */
	float BuiltFloorLampIntensity = 0.f;
	float BuiltFloorLampRadius = 0.f;

	/** Scalability::OnScalabilitySettingsChanged, so a live quality change re-tunes without a rebuild. */
	FDelegateHandle ScalabilityChangedHandle;
};
