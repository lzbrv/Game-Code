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

#include "TraceArenaBuilder.generated.h"

class ADirectionalLight;
class ASkyLight;
class ATraceEndzone;
class ATraceTeamPlayerStart;
class UBoxComponent;
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
 * A 33600 x 9600 uu Tron arena - LENGTH : WIDTH = 3.5 : 1. Inside it:
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
 *  - a lit gate spanning the full width of each endzone; the endzones themselves run sideline to
 *    sideline - see EndzoneHalfWidth;
 *  - and, for MODE B, a GOAL at each end: two posts 2000 uu apart (GoalHalfWidth) carrying a
 *    crossbar at ClampedGoalHeight(), with the goal mouth lit on the floor between them. See the
 *    two-modes note below.
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
 * That comes to ~1190 components on the shipped field, up from ~830 at 24000 x 9600: the field grew
 * 40% and the structure count with it, plus the mode-B goal furniture.
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
 * ASSET DEPENDENCY
 * ----------------
 * The two materials live at /Game/Generated/Materials/ and are produced by
 * Scripts/generate_content.py, not committed (Content/Generated/ is gitignored - the repo stays
 * text-only, see the policy note at the top of that script). If they are missing the builder falls
 * back to /Engine/BasicShapes/BasicShapeMaterial and logs a warning: the arena still plays, it just
 * renders flat and lit instead of neon.
 */
UCLASS()
class TRACE_API ATraceArenaBuilder : public AActor
{
	GENERATED_BODY()

public:
	ATraceArenaBuilder();

	//~ Contract surface (spec section 7)

	/** First arena builder in @p World, or null. Cheap enough for occasional use, not per-tick. */
	static ATraceArenaBuilder* Get(UWorld* World);

	/** World-space point the Core spawns at and resets to: just above the centre pedestal. */
	FVector GetCoreSpawnLocation() const;

	/** World-space playable volume: floor to wall top, inside faces of the four walls. */
	FBox GetFieldBounds() const;

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
	 * Height of a goal volume, floor upward, from UTraceSettings::GoalHeightUU.
	 *
	 * 440 uu by default: spec v5 section 4 scaled it by the same 2000/3200 the width was scaled by,
	 * so "reduce height and width" kept the mouth's proportions instead of making it a letterbox.
	 *
	 * FINITE ON PURPOSE - "height finite so it reads as a goal rather than a wall". Clamped to the
	 * wall height at the top so the crossbar can never be built above the sky.
	 */
	float ClampedGoalHeight() const;

	/**
	 * World-space box of the GOAL at @p EndSign (-1 for the -X end, +1 for the +X end).
	 *
	 * Goal line to end wall along X (the same depth as the endzone, so a Core thrown to the back of
	 * the net still counts), +/- GoalHalfWidth() along Y, and floor to ClampedGoalHeight() in Z.
	 */
	FBox GetGoalBounds(float EndSign) const;

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
	 */
	void EnsureBuilt();

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
	 * Length of the field along X (goal to goal).
	 *
	 * 24000 -> 33600 for spec v4 section 3, verbatim: "Lengthen the map to a 3.5:1 ratio, adjusting
	 * the structures to match." 33600 : 9600 is exactly 3.5 : 1, and [ASSUMPTION] "lengthen" grows
	 * the long axis rather than narrowing the short one.
	 *
	 * The layout scales with this: the cover scatter, the corner banks, the pylons and the endzone
	 * gates are all placed at fractions of the half length, so 33600 is a tuning value rather than a
	 * load-bearing constant. Do not drop it below ~12000 or the centre diamond and the two spawn
	 * lines start to overlap.
	 *
	 * THE COST, STATED PLAINLY: at WalkSpeed 800 a full-field run is 42 seconds. If that plays badly
	 * the alternative reading of the same note is to NARROW instead - set this to 24000 and FieldWidth
	 * to 6857 and the arena rebuilds itself at 3.5 : 1 with a 30-second field. Nothing else needs
	 * touching; that is what the "everything is a fraction" rule is for. (Check HitscanRange either
	 * way - see FieldWidth.)
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float FieldLength = 33600.f;

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
	 * THE ONE NUMBER THAT DOES NOT LIVE HERE and must be re-checked by hand is
	 * UTraceSettings::HitscanRange, which has to clear the field diagonal. 33600 x 9600 is a 34944 uu
	 * diagonal, so the 28000 that covered the old 24000 x 9600 field NO LONGER REACHES: a shot down
	 * the long diagonal now dies 6944 uu short of a target the player can plainly see. That property
	 * lives in UTraceSettings (and, because the ini wins, in Config/DefaultGame.ini as well) and it
	 * needs to go to 36000. It is called out in this pass's report.
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
	 * THERE IS NO WIDTH DIAL, and that is deliberate: an endzone spans the ENTIRE width of the field,
	 * sideline to sideline, exactly like a real football endzone. See EndzoneHalfWidth().
	 *
	 * Read it through ClampedEndzoneDepth(), never raw - three separate places used to re-clamp this
	 * by hand and one of them getting it wrong is what put both teams' spawn lines on the centre dais.
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

	// --- Corner banks (the sketch's green arrows) ------------------------------------------------
	//
	// Four terraced banks, one per quadrant, raised along the long edges and stepping DOWN toward
	// the middle of the field. Read the sketch as a shallow stadium bowl: high at the corners,
	// flat in the centre. See BuildCornerBanks for the shape and for why it is terraced rather
	// than ramped.

	/** Master switch for the four terraced corner banks. */
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
	 * hidden, low enough that it never reads as a wall. The terrace count is derived from this so
	 * that every riser stays under UCharacterMovementComponent::MaxStepHeight whatever it is set
	 * to - see BuildCornerBanks.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Banks", meta = (ClampMin = "0.0"))
	float BankHeight = 352.f;

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

	UPROPERTY(EditAnywhere, Category = "Trace|Spawns")
	int32 StartsPerTeam = 5;

	// --- Lighting --------------------------------------------------------------------------------
	//
	// Exposure is pinned: Config/DefaultEngine.ini sets r.DefaultFeature.AutoExposure=False, so the
	// tonemapper sees scene radiance unscaled and every intensity below is an absolute number rather
	// than something auto-exposure will normalise away. Measure with -TraceAutoShot after changing
	// any of them; do not assume.

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
	 * The four terraced corner banks - the sketch's green arrows, i.e. the shallow stadium bowl.
	 *
	 * REPLACED the four stepped wing platforms and the two rows of segmented lane rails. Those
	 * existed to break up a 12000 uu wide field; at 9600 the banks do that job and do it as
	 * TERRAIN, which is what the sketch actually draws.
	 */
	void BuildCornerBanks(bool bBuildVisuals);

	void BuildCoverField(bool bBuildVisuals);
	void BuildFlanks(bool bBuildVisuals);
	void BuildEndzones(bool bBuildVisuals);

	/**
	 * The MODE B goals: two posts, a crossbar, a lit mouth on the floor and the scoring volume, at
	 * each end. Built unconditionally and then hidden if mode A is the one armed - see the two-modes
	 * note in the class comment.
	 */
	void BuildGoals(bool bBuildVisuals);

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
	 * Destroys everything BuildArena() made - every component attached under Root, every actor in
	 * SpawnedActors, every MID - and resets the built flag so the next build starts from nothing.
	 *
	 * Editor-only, and it stays that way on purpose: at runtime the arena is built exactly once per
	 * world and EndPlay is the only teardown path that exists.
	 */
	void DestroyBuiltArena();

	// --- Primitive helpers -----------------------------------------------------------------------

	/**
	 * Adds a visual-only mesh block. @p Size is the desired world size in uu; the engine basic
	 * shapes are 100 uu and centred on their pivot, so the scale is just Size/100.
	 * Returns null (harmlessly) if the mesh asset failed to resolve.
	 */
	UStaticMeshComponent* AddMeshBlock(UStaticMesh* Mesh, const FVector& LocalCenter, const FVector& Size,
		UMaterialInstanceDynamic* MID, bool bCastShadow, const TCHAR* DebugName, float YawDegrees = 0.f);

	/**
	 * Adds an invisible box collider. Collision deliberately lives in box components rather than in
	 * the static meshes: it costs less, and it means the floor and walls still exist even if
	 * /Engine/BasicShapes ever fails to resolve (a dedicated server build, for instance) - players
	 * would see nothing but would not fall out of the world.
	 */
	UBoxComponent* AddCollisionBlock(const FVector& LocalCenter, const FVector& Size, const TCHAR* DebugName,
		float YawDegrees = 0.f);

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

	/** Dark lit structural surface. All parameters are M_TraceSurface's; see generate_content.py. */
	UMaterialInstanceDynamic* MakeSurfaceMID(const FLinearColor& BaseColor, float Roughness,
		float Metallic = 0.f, const FLinearColor& Emissive = FLinearColor::Black, float EmissiveStrength = 1.f);

	/** Unlit neon. @p Glow multiplies @p Color; > 1 is what pushes it over the bloom threshold. */
	UMaterialInstanceDynamic* MakeNeonMID(const FLinearColor& Color, float Glow);

	// --- Derived layout --------------------------------------------------------------------------
	float HalfLength() const { return FieldLength * 0.5f; }
	float HalfWidth() const { return FieldWidth * 0.5f; }

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
	 */
	float PlayerHeightUU() const;

	/**
	 * |Y| at which the corner banks start, i.e. the half width of the FLAT central playfield.
	 *
	 * Everything laid on the floor (the grid, the lane stripes) and every piece of cover has to
	 * stay inside this or it would be built partly inside a bank - a floor decal buried under a
	 * terrace, or a 176 uu block with its bottom step swallowed. Returns HalfWidth() outright when
	 * the banks are switched off, so the same call site is correct either way.
	 */
	float BankInnerHalfWidth() const;

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
	 * One piece of geometry that belongs to a single scoring mode, plus the collision state it was
	 * built with, so hiding and re-showing it restores exactly what it had rather than a guess.
	 */
	struct FTraceModePiece
	{
		TWeakObjectPtr<USceneComponent> Component;

		/** Collision the component was built with. Restored verbatim when its mode is armed. */
		TEnumAsByte<ECollisionEnabled::Type> Collision = ECollisionEnabled::NoCollision;
	};

	/**
	 * Number of components attached under Root right now.
	 *
	 * Paired with CollectPiecesSince() this is how a build step tags everything it made without any
	 * of the primitive helpers knowing modes exist: mark, build, collect the difference. Every helper
	 * in this file attaches to Root and appends in order, so the difference IS what the step built.
	 */
	int32 MarkBuiltComponents() const;

	/** Appends everything attached under Root since @p Mark to @p Out, with its collision state. */
	void CollectPiecesSince(int32 Mark, TArray<FTraceModePiece>& Out) const;

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
	 * Repaints both endzones for a new side assignment (spec §1 half-time switch).
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

private:
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

	/** Endzone surfaces whose colour follows the side assignment. Built once, repainted at half time. */
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
	 * Every ATraceEndzone this builder spawned, both shapes, server only.
	 *
	 * Weak, and separate from SpawnedActors (which owns them): ApplyScoringMode has to arm one pair
	 * and disarm the other, and walking a typed list beats filtering the actor list by cast on every
	 * toggle. Also empty on clients, where no scoring volume is ever spawned.
	 */
	TArray<TWeakObjectPtr<ATraceEndzone>> ScoringVolumes;


	/** Engine basic shapes, resolved in the constructor so the cooker keeps them (contract §2). */
	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CylinderMesh;

	/** Fallback only: /Engine/BasicShapes/BasicShapeMaterial. Lit, no emissive input. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> BaseMaterial;

	/** /Game/Generated/Materials/M_TraceSurface - see the asset note in the class comment. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SurfaceMaterial;

	/** /Game/Generated/Materials/M_TraceNeon. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> NeonMaterial;

	/** Keeps the tint instances alive independently of the components they are assigned to. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> TintMIDs;

	/** Actors we spawned, so a destroyed builder takes its arena with it instead of orphaning it. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> SpawnedActors;

	/** BeginPlay can be forced early by the GameMode (DispatchBeginPlay); never build twice. */
	bool bArenaBuilt = false;

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
};
