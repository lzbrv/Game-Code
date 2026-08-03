// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"      // EComponentMobility, EEndPlayReason
#include "GameFramework/Actor.h"
#include "Math/Box.h"                // FBox
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"              // ETraceTeam

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
 * WHAT IT MAKES
 * -------------
 * A 24000 x 12000 uu Tron arena: a near-black glossy floor carrying a team-tinted neon grid, four
 * 2600 uu perimeter walls with lit trim and vertical ribs, a four-tier stepped centre dais with the
 * Core pedestal on top and four light pylons around it, four stepped wing platforms, two rows of
 * segmented lane rails that split the field into a central spine plus two lanes per side, a
 * scattering of diamond cover blocks, and a lit gate structure over each endzone. Along both
 * flanks: a row of wall buttresses carrying a continuous high rail, a light bridge per quadrant out
 * to the lane pylons, bright outer-lane floor stripes and a pylon in each corner. Plus the gameplay
 * furniture: two ATraceEndzone triggers, five ATraceTeamPlayerStarts per team, the lighting rig
 * (three directional lights plus a 24-lamp floor lattice), height fog and an unbound post-process
 * volume.
 *
 * WHY IT IS THIS BIG
 * ------------------
 * The field was 8000 x 4000. At WalkSpeed 720 that is 11 seconds end to end and a point was over
 * before it started. 24000 x 12000 is 3x linear / 9x area: a full-field carry is ~33 seconds, so
 * the carrier actually has to survive a journey and the dash-through-the-trail counterplay gets
 * time to happen. Every derived number below is expressed as a fraction of the field, so changing
 * FieldLength/FieldWidth moves the whole layout coherently.
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

	// ---------------------------------------------------------------------------------------------
	// Layout. All values in unreal units, all local to this actor's transform, so the whole arena
	// can be moved by moving the builder. Editable for tuning via a Blueprint subclass assigned to
	// ATraceGameMode::ArenaBuilderClass - see the net note in the class comment before changing any
	// of them at runtime.
	// ---------------------------------------------------------------------------------------------

	/**
	 * Length of the field along X (goal to goal).
	 *
	 * The layout scales with this: rails, cover, wings and endzone gates are all placed at
	 * fractions of the half length, so 24000 is a tuning value rather than a load-bearing constant.
	 * Do not drop it below ~12000 or the centre dais and the two spawn lines start to overlap.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float FieldLength = 24000.f;

	/** Width of the field along Y (sideline to sideline). Layout scales with this too. */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float FieldWidth = 12000.f;

	/**
	 * Wall height. Tall on purpose: on a 24000 uu field a 700 uu wall is a kerb, and the walls are
	 * the main thing standing between the camera and an empty black sky.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float WallHeight = 2600.f;

	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float WallThickness = 200.f;

	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float FloorThickness = 120.f;

	/** How far each endzone reaches in from its end wall. */
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

	/** Master switch for the interior layout (dais, wings, rails, cover, gates). */
	UPROPERTY(EditAnywhere, Category = "Trace|Layout")
	bool bBuildInteriorLayout = true;

	/**
	 * Master switch for the flank dressing: wall buttresses, the high rails they carry, the light
	 * bridges out to the lane pylons, the outer-lane floor stripes and the corner pylons.
	 *
	 * WHY IT EXISTS. The field is 12000 uu wide and every route worth taking used to run down the
	 * middle, so the outer thirds rendered as two black voids - one measured screenshot had an
	 * entirely black left half. Empty space at this scale does not read as "arena", it reads as
	 * "unfinished". Everything this builds is either flush against a side wall or above head height,
	 * so it fills the void without narrowing the lanes or giving the navmesh-less bots a pocket to
	 * grind in (see ATraceBotController).
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
	 * Height fog density. Low: enough to give the 24000 uu field depth, not enough to hide it.
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
	void BuildWingPlatforms(bool bBuildVisuals);
	void BuildLaneRails(bool bBuildVisuals);
	void BuildCoverField(bool bBuildVisuals);
	void BuildFlanks(bool bBuildVisuals);
	void BuildEndzones(bool bBuildVisuals);
	void BuildPlayerStarts();
	void BuildLighting();
	void BuildFloorLamps();
	void BuildPostProcess();

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
	 */
	void AddNeonBlock(const FVector& LocalCenter, const FVector& Size, float YawDegrees,
		UMaterialInstanceDynamic* BodyMID, UMaterialInstanceDynamic* NeonMID,
		bool bCollide, const TCHAR* DebugName,
		UMaterialInstanceDynamic* FaceNeonMID = nullptr, bool bVerticalTrim = true);

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

	/** Top of the centre dais, i.e. the surface the Core pedestal stands on. */
	float DaisTopZ() const;

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
};
