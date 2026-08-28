// Trace — the Core's LOOKS. See TraceCore.h for the model and TraceCore.cpp for the rules.
//
// WHAT IS IN HERE. Everything the Core is DRAWN with and nothing it is DECIDED by: the SPEC v31 §4
// pack model and its three clips, the v32 §3 halo and thrown trail, the carried-in-hand placement,
// the heart light, the emissive state machine, and the console knobs that A/B all of it. Roughly a
// thousand lines that RESTRUCTURE tranche D2 moved out of TraceCore.cpp verbatim — banners, essays
// and measurements included — because none of it is read by possession, the pass, the throw or the
// turnover.
//
// THE PHYSICS IS NOT IN THIS FILE, and that separation is the point rather than a filing decision.
// The essay below says it in the original's words: not one line here changes LooseLocation,
// LooseVelocity, the 22 uu collision sphere, the bounce, the rest rule or the turnover geometry. A
// reader who wants to know why the ball MOVES the way it does never has to open this file, and a
// reader changing how it LOOKS cannot accidentally change how it moves.
//
// Everything here is a member of ATraceCore, so the split costs nothing in access: the state still
// lives on the one actor and these functions still reach it directly. The shared constant tables
// (TraceCoreArt, TraceCoreTuning) are in TraceCoreInternal.h, which the harness reads too.

#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceCoreInternal.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Audio/TraceAudio.h"
#include "UObject/ObjectKey.h"
#include "Net/UnrealNetwork.h"

#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TraceMatchTypes.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceEndzone.h"
#include "Gameplay/TraceFxShapes.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTrailComponent.h"
#include "World/TraceArenaBuilder.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

// The two team questions are shared with TraceCore.cpp, so both files ask them of the one
// implementation and every call site below reads exactly as it did when it lived there.
using namespace TraceCoreLocal;

// =================================================================================================
// *** SPEC v31 §4 — THE NEW CORE MODEL. WHAT THE THREE CLIPS ACTUALLY CONTAIN, MEASURED FROM
// *** Art/Pack/models/core.glb RATHER THAN READ OFF unreal-core_README.md.
// =================================================================================================
//
// The owner's words: "Implement the new core model with its idle animation. it is football shaped,
// but should have normal ball physics (just orienting the football to point with its velocity). It
// should also stand up on its pointy edge and rotate when on the ground."
//
// THE PHYSICS IS NOT TOUCHED BY ANY OF THIS. Not one line below changes LooseLocation, LooseVelocity,
// the 22 uu collision sphere, the bounce, the rest rule or the turnover geometry: the Core still
// sweeps and bounces as a sphere, which is what "normal ball physics ... do not turn it into a rugby
// ball that bounces oddly" asks for. Only a COMPONENT-RELATIVE ROTATION and a choice of clip change.
// The actor's own rotation is left at identity throughout, because the beacon shaft hangs off the
// same Root and has to stay a true vertical wherever the ball is pointing.
//
// --- THE CLIPS, PER-KEY, IN glTF AXES AND THEN IN UE ONES -----------------------------------------
//
// Interchange maps UE = (gl.x, gl.z, gl.y), so the mesh's long axis is UE local +X (nose at
// +18.5 uu, rear at -18.5, heart at the origin) and the authored "up" is UE local +Z.
//
//   Idle    3.600 s, loop.  `core` turns ONE FULL REVOLUTION about the authored UP axis (glTF +Y,
//                           i.e. UE local +Z), plus a +/-1.2 uu vertical bob and a 9 deg wobble; the
//                           shell halves shiver 4 mm; each cage ring turns once about the long axis.
//                           *** THIS IS A BALL LYING ON ITS SIDE ON A TURNTABLE. *** The README calls
//                           it a "slow tumble"; the keys say otherwise, and the keys win.
//   Pickup  0.550 s, one-shot.  Shell halves crack +/-30 mm apart (UE local +/-Y), the ball rises
//                           2 cm and turns 51.6 deg about the same up axis, rings turn ~34 deg.
//   Throw   0.500 s, loop.  `core` rolls EXACTLY FOUR WHOLE TURNS ABOUT THE LONG AXIS (glTF +X = UE
//                           local +X) - i.e. 8.00 rev/s at play rate 1 - and each cage ring turns
//                           once. Shell stays shut. This is the rifle spin, and it tiles.
//
// (The first read of Throw sampled its first, middle and last key and saw identity at all three -
// because 0, 2 and 4 whole turns ARE identity. That is this spec's "beware per-frame readers of fast
// quantities" in its purest form, and the numbers above come from accumulating every key delta
// instead. If you edit this block, measure the same way.)
//
// --- HOW EACH STATE IS POSED, AND THE ONE PLACE THE PACK COULD NOT BE FOLLOWED --------------------
//
//   CARRIED  component rotation IDENTITY, so the clip's spin axis lines up with world up and the
//            authored Idle turntable reads exactly as authored, floating over the holder's head.
//            Pickup plays once on the possession edge (the shell cracks, the heart is shown), then
//            Idle loops. That is the pack README's own instruction: "Play on grab, then hold the last
//            frame (or blend to your carry state)."
//
//   FLIGHT   component rotation = MakeFromX(velocity), so mesh +X - the nose, the throw axis and the
//            socket the pack documents - points along the velocity and keeps pointing along it as
//            gravity bends the arc. The roll is the authored Throw clip, at a play rate that turns
//            its authored 8.00 rev/s into the FX notes' "~10 rev/s". RELATIVE, per the standing rule:
//            the rate is (wanted rev/s) / (the clip's own rev/s), so a re-export at a different key
//            density still spins at the number the artist's notes ask for.
//
//   REST     *** THE ONE CONFLICT, AND IT IS GEOMETRIC, NOT A JUDGEMENT CALL. *** The owner wants the
//            ball STANDING ON ITS POINT and rotating. Standing it up means mesh +X points at world
//            +Z. A_Core_Idle's rotation is about the mesh's local +Z - which, once the ball is stood
//            up, is HORIZONTAL, so playing Idle in that pose does not spin the ball on its point, it
//            slowly cartwheels it end over end. The two cannot both be honoured.
//            The owner's sentence is explicit and specific ("a deliberate readable pose, not
//            physics"), so the pose wins, and the SPIN is taken from the one authored clip whose axis
//            of rotation IS the long axis - Throw - played at the turn rate A_Core_Idle authors for
//            the ground state (one revolution per 3.6 s). So the pose is the owner's, the motion is
//            still the artist's, and nothing here is hand-animated.
//            THE REAL FIX is an artist re-export of Idle with its spin on the long axis (or an
//            AnimBlueprint that masks the `core` bone's rotation); either would drop straight in here
//            and bring the bob and the shell shiver with it.
//
// --- SCALE ----------------------------------------------------------------------------------------
//
// DERIVED, NOT A LITERAL. The mesh imports life-size at 37.0 x 22.0 x 22.0 uu and the orb it replaces
// draws at 40 uu across (TraceModeBVisibleOrbRadius x 2), which is the size every mode-B surface,
// support-gap and turnover rule was tuned against - those rules are in TraceCore.cpp and
// TraceCoreTurnover.cpp, and they read the SAME constant out of TraceCoreInternal.h, which is what
// keeps the drawn ball and the rules that measure it one number. So the ball is drawn at exactly that
// length and the factor is (2 x TraceModeBVisibleOrbRadius) / (the mesh's own imported length),
// measured at BeginPlay - about x1.08 today. Nothing downstream has to be re-measured, a re-export at
// a different size cannot silently change the physics read, and both halves of the ratio are bases
// rather than magic numbers.
//
// The pack's first-person preview scales the Core to 0.30 m; that number is for a ball held in a
// hand, and this one is a world objective read across a 24000 uu field. Said out loud rather than
// silently ignored.
// =================================================================================================

/**
 * SPEC v31 §4. 1 (default): draw SK_TraceCore. 0: the pre-v31 engine sphere.
 *
 * The A/B for judging the new model against the old orb without a rebuild, and the switch that says
 * out loud that the fallback is a supported path rather than dead code. `-TraceNoCharacterArt` forces
 * the same fallback, so one command-line switch still shows the whole procedural game.
 */
static TAutoConsoleVariable<int32> CVarCorePackArt(
	TEXT("Trace.Core.PackArt"),
	1,
	TEXT("SPEC v31 §4. 1 (default): the Core is drawn as the pack's SK_TraceCore with its authored ")
	TEXT("clips. 0: the pre-v31 engine sphere. Also forced to 0 by -TraceNoCharacterArt and whenever ")
	TEXT("the pack art is not on disk (a clone with no `git lfs pull`)."),
	ECVF_Default);

/** SPEC v31 §4. Revolutions per second the Core turns at while standing on its point. */
TAutoConsoleVariable<float> CVarCoreRestSpin(
	TEXT("Trace.Core.RestSpinRevPerSecond"),
	TraceCoreArt::IdleClipRevPerSecond,
	TEXT("SPEC v31 §4. How fast the Core turns while standing on its point on the ground. Defaults to ")
	TEXT("the turn rate A_Core_Idle itself authors for the ground state (one revolution per 3.6 s), ")
	TEXT("expressed as a play rate on A_Core_Throw - the only authored clip whose spin axis is the ")
	TEXT("long axis, which is the axis a ball standing on its point has to turn about."),
	ECVF_Default);

/** SPEC v31 §4 / unreal-fx_README. Revolutions per second of the in-flight rifle spin. */
TAutoConsoleVariable<float> CVarCoreFlightSpin(
	TEXT("Trace.Core.FlightSpinRevPerSecond"),
	TraceCoreArt::FlightSpinRevPerSecond,
	TEXT("SPEC v31 §4. In-flight rifle spin about the long axis. 10 is the FX notes' number; the clip ")
	TEXT("itself authors 8.00 rev/s, so the default runs A_Core_Throw at play rate 1.25. Set 8 to play ")
	TEXT("the clip exactly as authored."),
	ECVF_Default);

/** SPEC v31 §4 / unreal-fx_README. The heart light's reach, uu. 0 switches the light off. */
static TAutoConsoleVariable<float> CVarCoreHeartLightRadius(
	TEXT("Trace.Core.HeartLightRadius"),
	900.f,
	TEXT("SPEC v31 §4. Attenuation radius of the #FF8A1F point light at the Core's heart socket - ")
	TEXT("\"it's what tells the other team who has the objective\", so the pack asks for it to be a ")
	TEXT("gameplay-tunable. 0 switches the light off entirely."),
	ECVF_Default);

/**
 * SPEC v32 §3. 1 (default): draw the pickup halo and the thrown trail. 0: neither.
 *
 * A CONSOLE VARIABLE AND NOT A UTraceSettings CONFIG KNOB, deliberately, and the same call the rest
 * of this block already made: Trace.Core.PackArt, Trace.Core.RestSpinRevPerSecond,
 * Trace.Core.FlightSpinRevPerSecond and both heart-light knobs are CVars too. The house rule
 * ("every new knob is UPROPERTY(config) in TraceSettings.h") is about GAMEPLAY tunables that a
 * designer sets and ships; this is the A/B switch for a piece of decoration, it changes no rule, and
 * putting it in the settings panel would advertise it as something a player should have an opinion
 * about. Said out loud here rather than left as an omission.
 */
static TAutoConsoleVariable<int32> CVarCoreFxGeometry(
	TEXT("Trace.Core.FxGeometry"),
	1,
	TEXT("SPEC v32 §3. 1 (default): draw unreal-fx_README's two pieces of Core geometry - the ")
	TEXT("one-shot pickup halo (icosphere r 20 uu, 0.6 -> 2.1x over the Pickup clip) and the tapered ")
	TEXT("thrown trail (r 5.5 -> 1.2 uu, peaking mid-flight). 0: neither, for the A/B."),
	ECVF_Default);

/**
 * SPEC v32 §3. How long the thrown trail is AT THE APEX, uu. Shorter everywhere else; see §3's
 * "peaking mid-flight".
 *
 * The FX doc gives the trail's two RADII and no length, so this is the one number in §3 that had to
 * be chosen rather than converted. 240 uu is six ball-lengths (the ball is drawn 40 uu long), which
 * at the mode-B throw speeds this file already tunes for - 1500-2162 uu/s today, and 1500-2236 when
 * this was chosen - is about a tenth of a second of travel. That is the streak length a real motion
 * blur would give, which is the effect the doc is describing. (Patch 28 §4 took CoreThrowSpeed
 * 3300 -> 2900, so the full-charge post-weight speed is 2161.5 uu/s. The conclusion is unchanged,
 * which is why the number moved and the length did not.)
 */
TAutoConsoleVariable<float> CVarCoreThrownTrailLength(
	TEXT("Trace.Core.ThrownTrailLengthUU"),
	240.f,
	TEXT("SPEC v32 §3. Length of the Core's thrown trail AT THE APEX of the arc, in uu; shorter at ")
	TEXT("the ends of the flight. The FX doc gives the trail's radii (0.055 -> 0.012 m) but no ")
	TEXT("length, so this is the one figure in §3 that is a choice: 240 uu is six ball-lengths."),
	ECVF_Default);

/** SPEC v31 §4. Heart light brightness while CARRIED; a loose Core gets a fraction of it. */
static TAutoConsoleVariable<float> CVarCoreHeartLightIntensity(
	TEXT("Trace.Core.HeartLightIntensity"),
	4000.f,
	TEXT("SPEC v31 §4. Unitless intensity of the heart light while the Core is CARRIED. A loose Core ")
	TEXT("is lit at a third of it - it is a marker, not a carrier's tell."),
	ECVF_Default);

/**
 * THE A/B FOR THE CARRIED BALL, and it exists so that the before and the after come out of ONE
 * binary with one flag changed - the standing rule this file already follows with Trace.Core.PackArt
 * and Trace.Core.FxGeometry.
 *
 * 0 restores the pre-fix picture EXACTLY: ArtRoot parked at zero, so the ball is back at OrbHeight
 * over the holder's head, and every drawn piece back to bOwnerNoSee, so the holder cannot see it.
 * That is the arm that reproduces the defect, and it has to keep working or the comparison frames
 * are two runs of two different builds rather than an A/B.
 */
static TAutoConsoleVariable<int32> CVarCoreCarryInHand(
	TEXT("Trace.Core.CarryInHand"),
	1,
	TEXT("1 (default): a carried Core is drawn in the holder's right hand and is VISIBLE to the ")
	TEXT("holder, whose camera is in third person for exactly as long as they hold it. 0: the ")
	TEXT("pre-fix picture - the ball floats at OrbHeight over the head and is hidden from its own ")
	TEXT("holder. The A/B arm; the beacon is unaffected either way."),
	ECVF_Default);

/**
 * The cradle's three numbers, live, so the next pass can photograph and retune without a rebuild -
 * this file's own idiom (Trace.Core.HeartLightRadius, Trace.Core.FlightSpinRevPerSecond).
 *
 * uu, in the CARRIER'S ACTOR FRAME, and RELATIVE TO THE FIST (not to `hand_r`, which is the wrist).
 * See TraceCoreTuning::CarryCradle* for where the defaults come from.
 *
 * *** THESE THREE ARE BOUNDED. *** UpdateCarriedArtPlacement clamps the vector they make to the drawn
 * ball's own half-extent, so a tuning pass can move the ball AROUND the hand but cannot move it OUT
 * of the hand - which is exactly the failure the last set of defaults shipped. Setting any of them
 * past the shell is the red arm that proves the clamp runs: `Trace.Core.CarryOffsetUp 40` asks for a
 * ball 40 uu over the fist and Trace.Core.CarryProbe still reports contact.
 */
static TAutoConsoleVariable<float> CVarCoreCarryOffsetForward(
	TEXT("Trace.Core.CarryOffsetForward"),
	static_cast<float>(TraceCoreTuning::CarryCradleForward),
	TEXT("uu the carried ball sits AHEAD of the holder's closed right fist, in the holder's own ")
	TEXT("frame. Clamped with the other two to the ball's drawn half-extent."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCoreCarryOffsetRight(
	TEXT("Trace.Core.CarryOffsetRight"),
	static_cast<float>(TraceCoreTuning::CarryCradleRight),
	TEXT("uu the carried ball sits OUTBOARD of the holder's closed right fist. What this number buys ")
	TEXT("is thigh clearance: the fist is 25.8 uu outboard and the ball's cross-radius is 11.9 uu, so ")
	TEXT("6 puts the inboard shell at 19.9 uu, clear of the leg. Clamped."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCoreCarryOffsetUp(
	TEXT("Trace.Core.CarryOffsetUp"),
	static_cast<float>(TraceCoreTuning::CarryCradleUp),
	TEXT("uu the carried ball sits ABOVE the holder's closed right fist, so the hand is buried in the ")
	TEXT("lower shell rather than hanging under it. Clamped."),
	ECVF_Default);

/**
 * The holder's right-hand socket, IN THE MANNEQUIN'S VOCABULARY. The third-person knife rig hangs
 * off the same one and is spelled the same way, and both go through
 * ATraceCharacter::ResolveBodyBoneName rather than reading the mesh directly: nine of the ten
 * characters wear the Mannequin, but Rocco's rig shares no bone name with it and calls this joint
 * `RightHand1`. The resolver keeps the existence check that has to happen either way - asking a mesh
 * for a socket it does not have returns the COMPONENT ORIGIN rather than failing, so an unguarded
 * read draws the ball lying at the pawn's feet and nothing in the log says why.
 *
 * A TCHAR literal rather than a file-scope `static const FName`: an FName built during static
 * initialisation runs before the name table is guaranteed to exist, and a name is cheap to build at
 * the one place that needs it.
 */
static const TCHAR* const GCarryHandSocketName = TEXT("hand_r");

/**
 * The forearm the hand hangs off, used ONLY to point `hand_r` -> fist in the direction the arm is
 * actually lying this frame. Spelled the Mannequin's way and resolved per-rig alongside the hand
 * (Rocco's is `RightForeArm1`). Optional by design: if it is missing the fist falls back to straight
 * down, which is where ABP_Unarmed's rest arm puts it anyway. Every humanoid rig this project can
 * load has it, so it is not a guess about one skeleton the way a finger bone would be.
 */
static const TCHAR* const GCarryForearmBoneName = TEXT("lowerarm_r");



// =================================================================================================
// Presentation
// =================================================================================================

void ATraceCore::ApplyAttachment()
{
	AppliedHolder = Carrier;
	bAppliedPassActive = bPassActive;
	bAppliedLoose = bLoose;
	bAppliedEver = true;

	if (IsValid(Carrier))
	{
		if (GetAttachParentActor() != Carrier)
		{
			AttachToActor(Carrier, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		}

		// Straight up the capsule axis, so the holder's yaw cannot swing the orb around and the
		// beacon is a true vertical wherever they are facing.
		SetActorRelativeLocation(FVector(0.0, 0.0, TraceCoreTuning::OrbHeight));
		SetActorRelativeRotation(FRotator::ZeroRotator);
	}
	else
	{
		if (GetAttachParentActor() != nullptr)
		{
			DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}
	}

	bAppliedTurnoverActive = IsTurnoverActive();

	// The beacon is placed in the actor's own space, once, from its two end heights.
	if (Beacon != nullptr && Beacon->GetStaticMesh() != nullptr)
	{
		const FBoxSphereBounds Bounds = Beacon->GetStaticMesh()->GetBounds();
		const double MeshHeight = FMath::Max(1.0, 2.0 * Bounds.BoxExtent.Z);
		const double MeshWidth = FMath::Max(1.0, 2.0 * Bounds.BoxExtent.X);

		const double Height = TraceCoreTuning::BeaconTop - TraceCoreTuning::BeaconBottom;
		const double Centre = (TraceCoreTuning::BeaconTop + TraceCoreTuning::BeaconBottom) * 0.5
			- TraceCoreTuning::OrbHeight;   // Relative to the actor, which sits at OrbHeight.

		// SPEC v25 §2/§3: "the beam ... should change colors to the opposite team and be LARGER for the
		// 5 seconds". A MULTIPLIER of the normal width, never a width of its own — Demo 21's standing
		// rule, because "larger" is a claim about the beam beside it and an absolute would stop being
		// larger the day TraceCoreTuning::BeaconWidth moved. The colour half is in UpdateVisuals().
		const double Width = TraceCoreTuning::BeaconWidth
			* (bAppliedTurnoverActive ? static_cast<double>(GetTurnoverBeamScale()) : 1.0);

		Beacon->SetRelativeLocation(FVector(0.0, 0.0, Centre));
		Beacon->SetRelativeScale3D(FVector(
			Width / MeshWidth,
			Width / MeshWidth,
			Height / MeshHeight));

		// Shown while somebody is holding it - a holderless Core is a kickoff, not a target - and, in
		// mode B, while it is LOOSE, which is the one moment in that mode when every player on the
		// field needs to find it from wherever they happen to be standing.
		Beacon->SetVisibility(IsValid(Carrier) || bLoose);
	}

	// FPrimitiveSceneProxy caches the actor owner chain when it is BUILT, and that chain is what
	// bOwnerNoSee is resolved against. The chain changes every time the Core changes hands (GrantTo
	// calls SetOwner), and SetOwnerNoSee(true) would early-out because the flag itself is unchanged
	// - so the proxy has to be rebuilt explicitly or the previous holder would keep the Core hidden
	// from themselves while the new one stared straight at it.
	//
	// PackMesh AND PickupHalo JOIN THAT LIST, in MarkDrawnPiecesRenderStateDirty. They were missing,
	// and it was invisible only because nothing had ever shown the ball to its own holder: PackMesh
	// is the component that actually draws the Core whenever the pack art resolved (Mesh is only the
	// fallback sphere), so the one piece whose owner chain most needed re-resolving was the one piece
	// not being told to. Now that a carried ball is deliberately unhidden for its holder, a stale
	// proxy would mean the PREVIOUS holder kept seeing a ball they no longer have.
	MarkDrawnPiecesRenderStateDirty();

	// THE PICTURE FOLLOWS THE POSSESSION ON THE SAME FRAME, not on the next tick. ApplyAttachment is
	// the funnel every possession change goes through on every machine; placing the art from Tick
	// alone would leave one frame in which the ball had changed hands and was still being drawn in
	// the old holder's hand - and one frame is exactly what a screenshot catches.
	UpdateCarriedArtPlacement();
	UpdateCarriedArtVisibility();
}

void ATraceCore::MarkDrawnPiecesRenderStateDirty()
{
	// Every VISIBLE piece of this actor, in one list, because bOwnerNoSee is resolved against an
	// owner chain the scene proxy CACHES when it is built - so a piece left out of this list is a
	// piece whose owner-visibility silently keeps the previous holder's answer.
	for (UPrimitiveComponent* Piece : { static_cast<UPrimitiveComponent*>(Mesh.Get()),
	                                    static_cast<UPrimitiveComponent*>(PackMesh.Get()),
	                                    static_cast<UPrimitiveComponent*>(PickupHalo.Get()),
	                                    static_cast<UPrimitiveComponent*>(Beacon.Get()) })
	{
		if (Piece != nullptr)
		{
			Piece->MarkRenderStateDirty();
		}
	}
}

double ATraceCore::GetDrawnBallHalfExtentUU() const
{
	// THE NARROW HALF-EXTENT, AND DELIBERATELY SO. The pack ball is a 40.0 x 23.8 x 23.8 uu football
	// and the Carried state leaves PackMesh's component rotation at identity, so its long axis is
	// wherever A_Core_Idle's turntable has swung it this instant. Sizing the grip off the LONG axis
	// would hold the hand only at the spin angles that happen to point it at the fist; sizing it off
	// the narrow one holds at every angle, and at the other angles it holds deeper. The worst case is
	// the honest case.
	const bool bDrawPack = bPackArtActive && CVarCorePackArt.GetValueOnGameThread() != 0;
	if (bDrawPack && PackMesh != nullptr && PackMesh->GetSkeletalMeshAsset() != nullptr)
	{
		// Imported bounds x the SAME derived scale the mesh is actually drawn at, so a re-export at a
		// different size moves the grip with it instead of silently un-holding the ball. That is the
		// same rule the drawn length itself follows - see the ART block's SCALE note.
		const FVector Extent = PackMesh->GetSkeletalMeshAsset()->GetImportedBounds().BoxExtent;
		return FMath::Max(1.0, Extent.GetMin() * static_cast<double>(PackArtScale));
	}

	// The fallback path draws /Engine/BasicShapes/Sphere at OrbScale, which is the 20 uu radius every
	// mode-B rule is written against (TraceCore.cpp / TraceCoreTurnover.cpp, through the same
	// TraceModeBVisibleOrbRadius this line returns).
	return TraceModeBVisibleOrbRadius;
}

void ATraceCore::UpdateCarriedArtPlacement()
{
	if (ArtRoot == nullptr)
	{
		return;
	}

	// NOT CARRIED, OR THE A/B ARM IS OFF: dead centre on the actor, which is bit-identical to every
	// build before this one. A loose, thrown or resting Core is not being held by anybody and there
	// is no hand to put it in.
	FVector Wanted = FVector::ZeroVector;
	bool bHandResolved = false;

	// Cleared here, not only written in the carried branch: a stale "the fist is 1.9 uu inside the
	// shell" left over from the last holder is a probe line that lies about a ball lying on the floor.
	CarryFistDepthUU = 0.0;
	CarryCradleClampedUU = 0.0;
	CarryHandBoneResolved = NAME_None;
	CarryHandBodyName.Reset();

	// The FIST, in the holder's own frame - resolved for anybody who has the Core, whether or not the
	// A/B arm is going to use it. It is what the grip measurement below is taken against, and a
	// measurement that only exists on the arm it is meant to defend cannot report that arm failing.
	FVector FistLocal = FVector::ZeroVector;
	bool bFistKnown = false;

	ATraceCharacter* const Holder = Carrier;
	const bool bCarryArm = CVarCoreCarryInHand.GetValueOnGameThread() != 0;
	if (IsValid(Holder))
	{
		// THE HAND, IN THE HOLDER'S OWN FRAME. Read as a WORLD transform and un-rotated back into the
		// holder's frame rather than taken from the bone's local axes: the offset below is authored in
		// forward/right/up and that is only true of the actor's axes. This project's knife rig had to
		// MEASURE its hand-space cant because a bone's axes did not read the way anyone expected, and
		// a ball is a sphere - it cannot show a reader that its frame is wrong.
		FVector HandLocal(0.0, TraceCoreTuning::CarryHandRestRight, 0.0);

		// Straight down is the fallback direction for "further along the arm", because that is where
		// ABP_Unarmed's rest arm points and it is the only answer available without a second bone.
		FVector AlongArmLocal(0.0, 0.0, -1.0);

		if (const USkeletalMeshComponent* Body = Holder->GetMesh())
		{
			// *** ASKED FOR IN THE MANNEQUIN'S VOCABULARY, RESOLVED AGAINST THE BODY ACTUALLY ON THIS
			// PAWN. *** ResolveBodyBoneName answers `hand_r` on the nine Mannequin characters and
			// `RightHand1` on Rocco, whose rig shares no bone name with the Mannequin's; a raw
			// DoesSocketExist here dropped a Rocco carrier's ball to the hip fallback. It still does
			// the existence check itself - NAME_None is "this rig has no right hand under any name
			// this project knows" - so the guard that stands between us and GetSocketTransform's habit
			// of answering a missing socket with the COMPONENT ORIGIN has not been given up.
			const FName HandSocket = Holder->ResolveBodyBoneName(FName(GCarryHandSocketName));

			// Recorded whether or not it resolved, and BEFORE the branch, so the probe can name the
			// rig it failed on as readily as the one it worked on. The name is left EMPTY rather than
			// GetNameSafe's "None" when there is no asset at all, because "None" is also a plausible
			// mesh name to a reader and the two cases print differently below.
			if (const USkeletalMesh* const HolderMesh = Body->GetSkeletalMeshAsset())
			{
				CarryHandBodyName = HolderMesh->GetName();
			}
			CarryHandBoneResolved = HandSocket;

			if (!HandSocket.IsNone())
			{
				const FVector HandWorld = Body->GetSocketTransform(HandSocket, RTS_World).GetLocation();
				HandLocal = Holder->GetActorRotation().UnrotateVector(HandWorld - Holder->GetActorLocation());
				bHandResolved = true;

				// THE ARM'S OWN DIRECTION, from two joint POSITIONS rather than from a bone's axes -
				// same reason the hand itself is read as a world position and un-rotated: this file's
				// knife rig had to MEASURE its hand-space cant because the axes did not read the way
				// anyone expected, and elbow -> wrist is a direction no convention can flip.
				const FName ForearmBone = Holder->ResolveBodyBoneName(FName(GCarryForearmBoneName));
				if (!ForearmBone.IsNone())
				{
					const FVector ElbowWorld = Body->GetSocketTransform(ForearmBone, RTS_World).GetLocation();
					const FVector Along = (HandWorld - ElbowWorld).GetSafeNormal();
					if (!Along.IsNearlyZero())
					{
						AlongArmLocal = Holder->GetActorRotation().UnrotateVector(Along);
					}
				}
			}
		}

		// *** THE ANCHOR IS THE FIST, AND `hand_r` IS THE WRIST. *** Carrying on down the forearm by
		// CarryFistReach is what the last pass was missing: it hung the ball off the wrist joint, put
		// the whole closed hand between the anchor and the ball, and photographed as an orb floating
		// clear of an empty fist. The reach follows the live arm, so this stays true through the run
		// cycle rather than only while the carrier stands still.
		FistLocal = HandLocal + AlongArmLocal * TraceCoreTuning::CarryFistReach;
		bFistKnown = true;

		if (bCarryArm)
		{
			// Live, so the next pass can retune from the console against a running match rather than
			// from a rebuild - and so the three numbers that decide where ON the hand the ball sits
			// are the three numbers a reviewer can move.
			FVector Cradle(
				CVarCoreCarryOffsetForward.GetValueOnGameThread(),
				CVarCoreCarryOffsetRight.GetValueOnGameThread(),
				CVarCoreCarryOffsetUp.GetValueOnGameThread());

			// *** WHETHER THE BALL IS HELD IS ARITHMETIC, NOT TASTE, SO IT IS ENFORCED HERE. ***
			//
			// The fist has to end up INSIDE the drawn shell. The half-extent is read off the ball that
			// is actually on screen this frame (GetDrawnBallHalfExtentUU), not off a constant, because
			// that is precisely the mistake being fixed: the old defaults were reasoned against the
			// 20 uu engine sphere the pack ball replaced, and the pack ball is only 11.9 uu across its
			// narrow axis. A clamp rather than a compile-time assert because the three inputs are
			// console variables and the ball's size is a property of an asset - neither of which is
			// known at build time.
			const double MaxReach = FMath::Max(1.0,
				GetDrawnBallHalfExtentUU() - TraceCoreTuning::CarryGripBite);
			if (Cradle.SizeSquared() > MaxReach * MaxReach)
			{
				CarryCradleClampedUU = Cradle.Size() - MaxReach;
				Cradle = Cradle.GetSafeNormal() * MaxReach;
			}

			// *** MINUS OrbHeight, AND THAT SUBTRACTION IS THE WHOLE TRICK. *** The ACTOR is already
			// 150 uu up the capsule axis and it is staying there, because that is the position the
			// beacon arithmetic, the ArtShots camera and the teleport audit are all written against.
			// ArtRoot is a relative offset FROM the actor, so cancelling OrbHeight here is what lets
			// the picture come down to hand height while the thing the game reads has not moved.
			Wanted = FistLocal + Cradle - FVector(0.0, 0.0, TraceCoreTuning::OrbHeight);

			// A DEGRADE, NOT AN ERROR, AND IT SAYS SO ONCE. A pawn drawing no body at all - a machine
			// with no character art imported, which draws the fallback capsule - has no right hand to
			// find; so would a future rig whose spelling is in neither the Mannequin's vocabulary nor
			// ResolveBodyBoneName's alias table. The ball then sits at that pawn's right hip on the
			// measured rest offset, which is a worse picture and a working one. Silence would be the
			// bad outcome: this guard is what stands between us and GetSocketTransform's habit of
			// answering a missing socket with the COMPONENT ORIGIN, i.e. a ball at the feet.
			//
			// *** IT NAMES THE MESH IT FAILED ON, because the previous wording guessed at the cause
			// ("no mannequin import?") and was wrong the one time it ever fired: it fired on Rocco, on
			// a machine with the Mannequin installed, because this call site was not going through
			// the resolver. A reader needs the rig's name to know which of the two cases this is.
			//
			// Inside this branch so that turning the A/B arm off (Trace.Core.CarryInHand 0) cannot
			// produce a warning about a socket nothing asked for.
			if (!bHandResolved && !bCarryHandMissingLogged)
			{
				bCarryHandMissingLogged = true;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Carry] %s draws `%s`, which has no `%s` joint under that name or any alias ")
					TEXT("ResolveBodyBoneName knows; the carried Core falls back to the measured right-hip ")
					TEXT("offset (0, %.0f, 0) uu. The ball is drawn, just not in a hand."),
					*GetNameSafe(Holder),
					CarryHandBodyName.IsEmpty() ? TEXT("<no body mesh: art not imported>") : *CarryHandBodyName,
					GCarryHandSocketName, TraceCoreTuning::CarryHandRestRight);
			}
		}
	}

	bCarryHandSocketResolved = bHandResolved;

	// *** MEASURED OFF THE ANSWER, NOT OFF THE INPUTS. *** Taking the grip from the cradle vector
	// would only ever restate what was just asked for; taking it from where the ball ACTUALLY ends up
	// is what lets the same line report the A/B arm's own picture (Trace.Core.CarryInHand 0 leaves the
	// ball at OrbHeight and this prints roughly -150 uu, i.e. a ball a metre and a half from the hand)
	// and would have caught the defect this replaces. Wanted is relative to the ACTOR, which sits at
	// OrbHeight, so adding it back puts both terms in the holder's own frame.
	if (bFistKnown)
	{
		const FVector BallCentreLocal = Wanted + FVector(0.0, 0.0, TraceCoreTuning::OrbHeight);
		CarryFistDepthUU = GetDrawnBallHalfExtentUU() - FVector::Dist(BallCentreLocal, FistLocal);
	}

	// A hand moves every frame, so this really does write a transform most frames while the Core is
	// held - but it must not write one while it is not, and the compare is what makes a resting Core
	// cost nothing at all.
	if (bArtRootOffsetApplied && AppliedArtRootOffset.Equals(Wanted, 0.01))
	{
		return;
	}
	AppliedArtRootOffset = Wanted;
	bArtRootOffsetApplied = true;

	// TRANSLATION ONLY. The rotation stays identity so UpdateCoreArt's Carried case keeps world up as
	// the turntable's spin axis - it says so in as many words, and a rotation here would silently
	// break a claim made in a different function.
	ArtRoot->SetRelativeLocation(Wanted);
}

void ATraceCore::UpdateCarriedArtVisibility()
{
	bool bShow = false;

	ATraceCharacter* const Holder = Carrier;
	if (IsValid(Holder) && CVarCoreCarryInHand.GetValueOnGameThread() != 0)
	{
		// *** GATED ON THE HOLDER'S OWN BODY, AND ON THE HOLDER'S OWN FLAG. ***
		//
		// The pull-back from first to third person takes 0.35 s and passes the camera THROUGH the
		// pawn; at the start of it the ball would be 66 uu from the lens. The character already
		// decides when its own body may be drawn to its own camera - SetOwnBodyHiddenFromOwner, which
		// tests the SMOOTHSTEPPED blend alpha, not the raw one - and it records that decision by
		// putting bOwnerNoSee on its mesh. Reading that flag back is the whole rule: the ball and the
		// body appear on the same frame and can never disagree, and there is no second copy of the
		// 0.2 threshold here to drift from the one in ATraceCharacter.
		//
		// (Comparing GetViewBlendAlpha() against 0.2 directly is the trap: eased 0.2 is raw 0.276, so
		// the raw comparison would reveal the ball before the body.)
		const USkeletalMeshComponent* Body = Holder->GetMesh();
		bShow = (Body == nullptr) || !Body->bOwnerNoSee;
	}

	if (bShow == bCarryArtShownToOwner)
	{
		return;
	}
	bCarryArtShownToOwner = bShow;

	// THE BALL, AND ONLY THE BALL. Beacon is not in this list and must not join it - see the header
	// for the frame arithmetic (a 42 px emissive column through the crosshair and the scoreboard).
	// ThrownTrailSegments are not in it either: a ball in a hand is not in flight.
	for (UPrimitiveComponent* Piece : { static_cast<UPrimitiveComponent*>(Mesh.Get()),
	                                    static_cast<UPrimitiveComponent*>(PackMesh.Get()),
	                                    static_cast<UPrimitiveComponent*>(PickupHalo.Get()) })
	{
		if (Piece != nullptr)
		{
			Piece->SetOwnerNoSee(!bShow);
		}
	}
}

#if !UE_BUILD_SHIPPING
void ATraceCore::DebugLogCarryState() const
{
	const UWorld* const World = GetWorld();
	const ATraceCharacter* const Holder = Carrier;

	// The BALL's position, which is now a different thing from the ACTOR's position, and printing
	// both together is the point: a reviewer has to be able to see at a glance that the picture moved
	// and the thing every gameplay rule reads did not.
	const FVector BallWorld = (ArtRoot != nullptr) ? ArtRoot->GetComponentLocation() : GetActorLocation();

	// NAMES, NOT A BOOLEAN, for the hand: `hand_r -> RightHand1 on SK_Rocco` is a line that cannot
	// read green on the wrong rig, and a bare "resolved" did exactly that while a Rocco's ball was
	// hanging off his hip. See CarryHandBoneResolved.
	const FString HandReport = bCarryHandSocketResolved
		? FString::Printf(TEXT("%s->%s on %s"), GCarryHandSocketName,
			*CarryHandBoneResolved.ToString(),
			CarryHandBodyName.IsEmpty() ? TEXT("<no body mesh>") : *CarryHandBodyName)
		: FString::Printf(TEXT("NOT resolved on %s (hip fallback)"),
			CarryHandBodyName.IsEmpty() ? TEXT("<no body mesh>") : *CarryHandBodyName);

	UE_LOG(LogTraceGame, Display,
		TEXT("[CarryProbe] CarryInHand=%d  carrier=%s  hand=%s  artRoot rel=%s  BALL world=%s  ACTOR world=%s"),
		CVarCoreCarryInHand.GetValueOnGameThread(),
		IsValid(Holder) ? *GetNameSafe(Holder) : TEXT("<nobody - the Core is not held>"),
		*HandReport,
		*AppliedArtRootOffset.ToCompactString(),
		*BallWorld.ToCompactString(),
		*GetActorLocation().ToCompactString());

	if (IsValid(Holder))
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[CarryProbe] holder at %s; the ball is %.1f uu from the capsule centre, %.1f uu ")
			TEXT("outboard and %.1f uu up in the holder's own frame."),
			*Holder->GetActorLocation().ToCompactString(),
			FVector::Dist(Holder->GetActorLocation(), BallWorld),
			AppliedArtRootOffset.Y,
			AppliedArtRootOffset.Z + TraceCoreTuning::OrbHeight);

		// *** THE LINE THAT SETTLES "HELD" RATHER THAN "VISIBLE". *** The v32 verifier's finding was
		// that the ball was on screen with sky between it and an empty fist, and no number printed
		// here could have caught that: every existing line said where the ball WAS, none said what it
		// was touching. This one is the grip, signed, in uu - positive is the fist's centroid inside
		// the drawn shell, negative is the air gap that was photographed. Its two arms:
		//   Trace.Core.CarryInHand 0   -> the pre-fix picture, and this prints a large NEGATIVE gap.
		//   Trace.Core.CarryOffsetUp 40 -> asks for a ball far over the fist; the clamp trims it and
		//                                 this still prints contact, with the trim shown.
		UE_LOG(LogTraceGame, Display,
			TEXT("[CarryProbe] GRIP: the ball's narrow half-extent is %.1f uu and the fist's centroid ")
			TEXT("is %+.1f uu inside the shell (%s)%s."),
			GetDrawnBallHalfExtentUU(),
			CarryFistDepthUU,
			(CarryFistDepthUU > 0.0) ? TEXT("HELD - hand under the surface")
			                         : TEXT("NOT HELD - open air between hand and ball"),
			(CarryCradleClampedUU > 0.0)
				? *FString::Printf(TEXT("; the authored cradle was %.1f uu too long and was clamped"),
					CarryCradleClampedUU)
				: TEXT(""));
	}

	// WHERE IT LANDS ON THIS MACHINE'S SCREEN, because the reason the ball was hidden in the first
	// place was that it landed 36 px under the crosshair. A number here is what says whether the new
	// position solves that or merely moves it.
	APlayerController* const PC = (World != nullptr && GEngine != nullptr)
		? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	if (PC != nullptr)
	{
		FVector ViewLocation = FVector::ZeroVector;
		FRotator ViewRotation = FRotator::ZeroRotator;
		PC->GetPlayerViewPoint(ViewLocation, ViewRotation);

		int32 SizeX = 0;
		int32 SizeY = 0;
		PC->GetViewportSize(SizeX, SizeY);

		FVector2D Screen = FVector2D::ZeroVector;
		const bool bOnScreen = PC->ProjectWorldLocationToScreen(BallWorld, Screen);

		UE_LOG(LogTraceGame, Display,
			TEXT("[CarryProbe] local camera at %s: the ball is %.1f uu away and projects to %s ")
			TEXT("(%.0f, %.0f) px on a %dx%d viewport, i.e. %+.0f, %+.0f px from centre."),
			*ViewLocation.ToCompactString(),
			FVector::Dist(ViewLocation, BallWorld),
			bOnScreen ? TEXT("") : TEXT("OFF SCREEN at"),
			Screen.X, Screen.Y, SizeX, SizeY,
			Screen.X - SizeX * 0.5f, Screen.Y - SizeY * 0.5f);
	}

	// bOwnerNoSee ON EVERY DRAWN PIECE, INCLUDING THE ONES THAT MUST STAY HIDDEN. A probe that only
	// printed the pieces the fix unhides could not catch the failure that matters most - the beacon
	// joining them and putting a 42 px emissive column through the holder's crosshair.
	auto Report = [](const TCHAR* Label, const UPrimitiveComponent* Piece)
	{
		if (Piece == nullptr)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[CarryProbe]   %s: <null>"), Label);
			return;
		}
		UE_LOG(LogTraceGame, Display, TEXT("[CarryProbe]   %s: visible=%d ownerNoSee=%d"),
			Label, Piece->IsVisible() ? 1 : 0, Piece->bOwnerNoSee ? 1 : 0);
	};
	UE_LOG(LogTraceGame, Display,
		TEXT("[CarryProbe] drawn pieces (packArt=%d, so %s is the ball this run):"),
		bPackArtActive ? 1 : 0, bPackArtActive ? TEXT("PackMesh") : TEXT("Mesh"));
	Report(TEXT("Mesh"), Mesh);
	Report(TEXT("PackMesh"), PackMesh);
	Report(TEXT("PickupHalo"), PickupHalo);
	Report(TEXT("Beacon"), Beacon);
}
#endif // !UE_BUILD_SHIPPING

void ATraceCore::UpdateVisuals()
{
	// SPEC v25 §2/§3. "When a turnover happens ... the beam of light coming from the core should
	// change colors to the OPPOSITE TEAM." A turned-over Core has no holder, so GetHolderTeam() is
	// None and the beam would otherwise be the neutral grey a kickoff uses. For the length of the
	// window it becomes the colour of the side that may now take it, which is the read the note asks
	// for: a player sees, from across the field, whose Core it currently is.
	const ETraceTeam BeamTeam = IsTurnoverActive() ? GetTurnoverPullingTeam() : GetHolderTeam();

	FLinearColor Color = TraceTeamColor(BeamTeam);
	Color.A = 1.f;

	const bool bColorChanged = !bColorApplied || !Color.Equals(AppliedColor, 0.001f);
	AppliedColor = Color;
	bColorApplied = true;

	// The pass window is the one moment an enemy can actually shoot the holder. Making the orb
	// visibly hotter for exactly those 0.5s is the read that turns the risk beat into something a
	// defender can act on rather than something only the passer knows about.
	const float GlowScale = bPassActive ? TraceCoreTuning::PassGlowMultiplier : 1.f;

	auto Push = [this, &Color, GlowScale, bColorChanged](UMaterialInstanceDynamic* Material, float BaseGlow)
	{
		if (Material == nullptr)
		{
			return;
		}
		if (bColorChanged)
		{
			Material->SetVectorParameterValue(TEXT("Color"), Color);
			Material->SetVectorParameterValue(TEXT("BaseColor"), Color);   // No-op if absent.
		}
		if (bMaterialIsNeon)
		{
			Material->SetScalarParameterValue(TEXT("Glow"), BaseGlow * GlowScale);
		}
	};

	Push(MeshMID, TraceCoreTuning::OrbGlow);
	Push(BeaconMID, TraceCoreTuning::BeaconGlow);
}

// =================================================================================================
// SPEC v31 §4 — the model. See the ART block at the top of THIS file for the measured clip contents
// (it moved here with the code it documents; the constructor is in TraceCore.cpp now).
// =================================================================================================

ETraceCoreArtState ATraceCore::ResolveCoreArtState() const
{
	if (!bLoose)
	{
		// Held, or holderless and parked at home. A parked Core takes the ground pose, which is the
		// "come and get me" read and is what a kickoff Core is doing.
		return IsValid(Carrier) ? ETraceCoreArtState::Carried : ETraceCoreArtState::Rest;
	}

	// LOOSE. THE TEST IS THE REPLICATED VELOCITY, NOT bLooseAtRest, and that is not an oversight:
	// bLooseAtRest is a server-only member and every client would otherwise have to guess. The server
	// ZEROES LooseVelocity on the same line it sets bLooseAtRest (twice, in ServerTickLooseCore, and
	// there is an ensure in that function asserting the pair never come apart), so the replicated
	// velocity carries the same fact to every machine at no extra cost on the wire.
	//
	// Real state, not a timer - the standing warning at the top of this spec. A Core one frame from
	// landing is still in flight here because it is still moving, which is what a player sees.
	return FVector(LooseVelocity).IsNearlyZero(1.0) ? ETraceCoreArtState::Rest : ETraceCoreArtState::Flight;
}

void ATraceCore::SetCoreArtAnim(UAnimSequence* Anim, bool bLooping, float PlayRate)
{
	if (PackMesh == nullptr || Anim == nullptr)
	{
		return;
	}

	// IDEMPOTENT, AND THAT IS THE WHOLE JOB. PlayAnimation() rewinds to frame 0, so calling it every
	// tick would pin A_Core_Throw to its first frame forever and the ball would never appear to spin -
	// the same class of defect as the SMG's shot frame never being sampled. The clip is started on a
	// STATE EDGE and left alone; only the play rate may be re-pushed, and only when it has moved.
	if (AppliedArtAnim != Anim || bAppliedArtAnimLooping != bLooping)
	{
		PackMesh->PlayAnimation(Anim, bLooping);
		AppliedArtAnim = Anim;
		bAppliedArtAnimLooping = bLooping;
		AppliedArtPlayRate = 0.f;   // Force the rate push below.
	}

	if (!FMath::IsNearlyEqual(AppliedArtPlayRate, PlayRate, 0.0005f))
	{
		PackMesh->SetPlayRate(PlayRate);
		AppliedArtPlayRate = PlayRate;
	}
}

void ATraceCore::UpdateCoreArtEmissive(ETraceCoreArtState State, float LocalTimeSeconds)
{
	if (CyanMID == nullptr && AmberMID == nullptr && HeartLight == nullptr)
	{
		return;
	}

	// unreal-core_README.md's own table, and unreal-fx_README's "The Core". DRIVEN BY STATE, NOT BY
	// THE CLIP - the pack asks for exactly that in bold ("drive it by state, not by the clip - the
	// core needs to read differently across the map"), which is also why none of this is baked into a
	// Curve Float on an AnimSequence.
	//
	// The breathing term is sin() of an ABSOLUTE clock rather than an accumulator. That is deliberate
	// and it is the safe direction of this spec's per-frame-reader warning: a stateless function of
	// wall time cannot drift, cannot double-advance on a hitch, and gives every machine in the session
	// the same phase for free.
	auto Breathe = [LocalTimeSeconds](float Lo, float Hi, float Hz) -> float
	{
		const float Phase = 0.5f * (1.f + FMath::Sin(LocalTimeSeconds * Hz * 2.f * PI));
		return Lo + (Hi - Lo) * Phase;
	};

	float Cyan = TraceCoreArt::IdleCyanLo;
	float Amber = TraceCoreArt::IdleAmberLo;

	switch (State)
	{
	case ETraceCoreArtState::Carried:
		Cyan = Breathe(TraceCoreArt::CarriedCyanLo, TraceCoreArt::CarriedCyanHi, TraceCoreArt::CarriedPulseHz);
		Amber = Breathe(TraceCoreArt::CarriedAmberLo, TraceCoreArt::CarriedAmberHi, TraceCoreArt::CarriedPulseHz);
		break;

	case ETraceCoreArtState::Flight:
		// "Thrown: cyan to 3.4x ... amber up to 2.6x peak." Held at the peak for the whole flight
		// rather than ramped: the flight is the shortest state there is and a ramp across it would
		// mean the peak the notes specify is never actually reached on a short throw.
		Cyan = TraceCoreArt::ThrownCyan;
		Amber = TraceCoreArt::ThrownAmber;
		break;

	case ETraceCoreArtState::Rest:
	default:
		Cyan = Breathe(TraceCoreArt::IdleCyanLo, TraceCoreArt::IdleCyanHi, TraceCoreArt::IdleBreathHz);
		Amber = Breathe(TraceCoreArt::IdleAmberLo, TraceCoreArt::IdleAmberHi, TraceCoreArt::IdleBreathHz);
		break;
	}

	// THE PICKUP FLARE. "amber flares to 4.6x as the shell cracks", over the 0.55 s of A_Core_Pickup.
	// Driven from the possession EDGE this machine saw, not from the clip's playhead: the clip is
	// 0.55 s long, which is 33 frames at 60 Hz, and reading a playhead that the animation system may
	// already have advanced is the exact failure this spec warns about twice. Elapsed-since-an-edge
	// only ever gets subtracted, so there is nothing to advance and nothing to sample too late.
	if (State == ETraceCoreArtState::Carried && ArtPickupAnim != nullptr)
	{
		const float FlareSeconds = ArtPickupAnim->GetPlayLength();
		const float Elapsed = LocalTimeSeconds - ArtStateStartTime;
		// A literal rather than KINDA_SMALL_NUMBER: the legacy math macros were re-spelled during the
		// 5.x line and this module must still compile on 5.4 - 5.8. Same rule as CoreGeometryEpsilon.
		if (Elapsed >= 0.f && Elapsed < FlareSeconds && FlareSeconds > 1.e-4f)
		{
			const float Alpha = 1.f - (Elapsed / FlareSeconds);
			Amber = FMath::Max(Amber, FMath::Lerp(Amber, TraceCoreArt::PickupAmberFlare, Alpha));
		}
	}

	auto PushIntensity = [](UMaterialInstanceDynamic* Material, float& Applied, float Value)
	{
		if (Material != nullptr && !FMath::IsNearlyEqual(Applied, Value, 0.002f))
		{
			Material->SetScalarParameterValue(TEXT("EmissiveIntensity"), Value);
			Applied = Value;
		}
	};

	PushIntensity(CyanMID, ArtEmissiveAppliedCyan, Cyan);
	PushIntensity(AmberMID, ArtEmissiveAppliedAmber, Amber);

	// TEAM TINT ON THE CYAN SLOT ONLY, which is what unreal-core_README asks for: "override
	// circuit_cyan's base colour per team ... and leave core_amber alone, so the heart always reads as
	// the objective." It also preserves the read the pre-v31 sphere had, where the orb's colour WAS
	// the possession. A Core that belongs to nobody goes back to the artist's own #25E6FF.
	//
	// The emissive colour is carried across with the tint rather than dropped: the pack folded the
	// KHR strength (cyan 1.5) into EmissiveColor so that EmissiveIntensity could mean "1.0 = at rest",
	// so a tint that wrote a bare team colour would quietly darken the ball by a third.
	if (CyanMID != nullptr)
	{
		const ETraceTeam Team = IsTurnoverActive() ? GetTurnoverPullingTeam() : GetHolderTeam();
		FLinearColor Tint = TraceCoreArt::CyanEmissive;
		if (Team != ETraceTeam::None)
		{
			Tint = TraceTeamColor(Team);
			Tint.A = 1.f;
		}

		if (!bArtTintApplied || !Tint.Equals(ArtAppliedTint, 0.001f))
		{
			CyanMID->SetVectorParameterValue(TEXT("BaseColor"), Tint);
			CyanMID->SetVectorParameterValue(TEXT("EmissiveColor"), Tint * TraceCoreArt::CyanEmissiveStrength);
			ArtAppliedTint = Tint;
			bArtTintApplied = true;
		}
	}

	// The heart light. "A point light at the heart socket tinted #FF8A1F sells the carrier's position
	// to the other team - worth exposing as a gameplay-tunable radius", so both numbers are CVars.
	if (HeartLight != nullptr)
	{
		const float Radius = FMath::Max(0.f, CVarCoreHeartLightRadius.GetValueOnGameThread());
		const bool bWanted = bPackArtActive && Radius > 1.f && (IsValid(Carrier) || bLoose);

		if (HeartLight->IsVisible() != bWanted)
		{
			HeartLight->SetVisibility(bWanted);
		}

		if (bWanted)
		{
			// A LOOSE Core is a marker; a CARRIED one is the tell. A third, relative to the carried
			// intensity rather than a second absolute - the standing rule, and it means one slider
			// still moves both.
			const float Base = FMath::Max(0.f, CVarCoreHeartLightIntensity.GetValueOnGameThread());
			const float Wanted = (State == ETraceCoreArtState::Carried)
				? Base * (Amber / FMath::Max(0.01f, TraceCoreArt::CarriedAmberHi))
				: Base / 3.f;

			HeartLight->SetAttenuationRadius(Radius);
			HeartLight->SetIntensity(Wanted);
		}
	}
}

// =================================================================================================
// SPEC v32 §3 — THE MISSING GEOMETRY.
//
// unreal-fx_README, "The Core", is the authority for both numbers and both shapes:
//
//   "Pickup: amber flares to 4.6x as the shell cracks, plus a one-shot icosahedron halo (r 0.20)
//    expanding 0.6 -> 2.1x and fading out over 0.55 s."
//   "Thrown: cyan to 3.4x; a tapered trail cylinder (r 0.055 -> 0.012) streams behind the ball,
//    peaking mid-flight."
//
// --- WHY THIS IS NOT UTraceTrailComponent, WHICH SPEC v32 §3 SAYS TO READ FIRST -------------------
//
// It was read first. UTraceTrailComponent draws a ribbon behind a moving thing, which is the right
// SHAPE, and it cannot carry this. Four of its own stated assumptions fail, in the order they bite:
//
//  1. ITS OWNER MUST BE A CHARACTER. GetOwnerCharacter() is Cast<ATraceCharacter>(GetOwner()) and
//     everything downstream - the dash trip test, the parry window, the head-grace stub, the
//     predicted head that is drawn for the carrier ALONE - resolves through it. The Core is an
//     AActor and has no capsule, no controller and no team of its own.
//
//  2. ITS POINTS ARE LETHAL, AND ITS INVARIANT IS "VISIBLE == LETHAL". The ribbon is drawn at
//     EXACTLY the lethal cross-section (2 x GetTraceTrailRadius() wide, GetTraceTrailHeight() tall)
//     and its file header states that as a rule with two shipped-and-fixed bugs behind it. A
//     COSMETIC trail routed through it would either be lethal - a thrown Core that kills people it
//     flies past, which §1 forbids outright ("nothing may change a single hit") - or it would break
//     the one invariant that file exists to defend.
//
//  3. IT HAS NO TAPER AND CANNOT HAVE ONE. Its cross-section is constant BECAUSE it is the lethal
//     cross-section (see 2). The FX doc's trail runs 5.5 -> 1.2 uu, which is a different shape by
//     construction, not by configuration.
//
//  4. IT IS SCOPED TO POSSESSION, AND A THROWN CORE HAS NO HOLDER. SetEmitting(false) WIPES the
//     trace, and "a mode-B throw" is named in its header as one of the events that funnels through
//     that wipe. The instant this trail must START is precisely the instant that component is
//     required to have nothing left.
//
// So: new geometry, out of the shared §1 library, and none of the trail component is touched.
//
// --- SPIN: ALREADY CORRECT, AND NOTHING HERE CHANGES IT ------------------------------------------
//
// §3 also asks whether the flight spins about the long axis at ~10 rev/s. IT ALREADY DOES, and the
// work was done in v31: UpdateCoreArt's Flight case sets the component rotation to
// MakeFromX(LooseVelocity) - mesh local +X is the nose and the long axis, so the nose points along
// the velocity and follows the arc down - and plays A_Core_Throw, whose keys were MEASURED to roll
// the ball four whole turns about that same local +X in 0.500 s (see the ART block). The play rate
// is Trace.Core.FlightSpinRevPerSecond / 8.00, defaulting to 10/8 = 1.25. Not a tumble, not the
// wrong axis, and rate-relative so a re-export cannot silently change it. CHANGED NOTHING.
// =================================================================================================

void ATraceCore::HideCoreArtGeometry()
{
	if (PickupHalo != nullptr && PickupHalo->IsVisible())
	{
		PickupHalo->SetVisibility(false);
	}
	for (UStaticMeshComponent* Segment : ThrownTrailSegments)
	{
		if (Segment != nullptr && Segment->IsVisible())
		{
			Segment->SetVisibility(false);
		}
	}

	// Forget the last pushed opacities so the next show re-pushes them. Without this a halo that was
	// hidden at opacity 0.4 and shown again at 0.4 would skip the SetGlow and come back at whatever
	// the material happened to be left holding.
	ArtHaloAppliedOpacity = -1.f;
	ArtTrailAppliedOpacity = -1.f;
}

void ATraceCore::UpdateCoreArtGeometry(ETraceCoreArtState State, float LocalTimeSeconds)
{
	if (CVarCoreFxGeometry.GetValueOnGameThread() == 0)
	{
		HideCoreArtGeometry();
		return;
	}

	// THE TINT THE BALL IS ALREADY WEARING, read back rather than re-derived. UpdateCoreArtEmissive
	// ran one line ago and wrote it; deriving the team a second time here would be a second copy of
	// the rule in ten lines of each other, and the two would drift the first time one was edited. A
	// Core that belongs to nobody - which a THROWN one does, by definition - keeps the artist's own
	// #25E6FF, so the common case for the trail is the untinted colour either way.
	const FLinearColor CyanTint = bArtTintApplied ? ArtAppliedTint : TraceCoreArt::CyanEmissive;

	// ---------------------------------------------------------------------------------------------
	// THE PICKUP HALO
	//
	// THE SAME EDGE THE AMBER FLARE USES, AND THAT IS THE POINT OF §3's "do not add a second,
	// differently-timed detector for one fact". ArtStateStartTime is stamped by UpdateCoreArt when the
	// art state changes and is only ever SUBTRACTED from, so there is nothing to advance, nothing to
	// drift and nothing to sample too late - the standing warning about per-frame readers of short
	// quantities, of which 0.55 s is one.
	//
	// The duration is A_Core_Pickup's own play length, read exactly as UpdateCoreArtEmissive reads it
	// for the flare, so the halo and the flare cannot end on different frames. (The clip measures
	// 0.550 s, which is the FX doc's number; if a re-export changes it, both follow together.)
	// ---------------------------------------------------------------------------------------------
	bool bHaloVisible = false;
	if (PickupHalo != nullptr && PickupHaloMID != nullptr && State == ETraceCoreArtState::Carried
		&& ArtPickupAnim != nullptr)
	{
		const float HaloSeconds = ArtPickupAnim->GetPlayLength();
		const float Elapsed = LocalTimeSeconds - ArtStateStartTime;
		if (Elapsed >= 0.f && Elapsed < HaloSeconds && HaloSeconds > 1.e-4f)
		{
			const float Progress = Elapsed / HaloSeconds;

			// "expanding 0.6 -> 2.1x" of r 20 uu, and the uu -> component-scale conversion is the
			// library's single named constant. Nothing here divides by 100.
			const float RadiusUU = TraceCoreArt::PickupHaloRadiusUU
				* FMath::Lerp(TraceCoreArt::PickupHaloScaleStart, TraceCoreArt::PickupHaloScaleEnd, Progress);
			const float Scale = UTraceFxShapes::ShapeScaleForRadiusUU(RadiusUU);

			PickupHalo->SetRelativeLocation(FVector::ZeroVector);   // Centred on the ball's heart.
			PickupHalo->SetRelativeScale3D(FVector(Scale));

			// "fading out over 0.55 s". Linear, which is what the doc says; on an ADDITIVE blend the
			// opacity IS the weight of the colour being added, so a linear fade really is a linear
			// fade rather than a gamma-shaped one.
			const float Opacity = 1.f - Progress;
			if (!FMath::IsNearlyEqual(ArtHaloAppliedOpacity, Opacity, 0.004f))
			{
				// AMBER, not cyan: the FX doc puts the halo in the same sentence as the amber flare
				// ("amber flares to 4.6x as the shell cracks, PLUS a one-shot icosahedron halo"), and
				// the shell cracking is an amber event on this ball - the heart light is #FF8A1F too.
				//
				// Intensity 1.0 and the fade carried entirely on the opacity, because on the additive
				// parent the two multiply into the same quantity and that parent has no Glow scalar,
				// so anything above 1.0 is silently clamped. Pushing the brightness through the knob
				// that actually exists is the honest use of this blend; see SetGlow's header.
				UTraceFxShapes::SetGlow(PickupHaloMID, PickupHaloBlend,
					TraceCoreArt::AmberSRGB, /*Intensity=*/1.f, Opacity);
				ArtHaloAppliedOpacity = Opacity;
			}

			bHaloVisible = true;
		}
	}

	if (PickupHalo != nullptr && PickupHalo->IsVisible() != bHaloVisible)
	{
		PickupHalo->SetVisibility(bHaloVisible);
		if (!bHaloVisible)
		{
			ArtHaloAppliedOpacity = -1.f;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// THE THROWN TRAIL
	//
	// *** "PEAKING MID-FLIGHT" WITHOUT INVENTING A SECOND CLOCK. ***
	//
	// The obvious reading - elapsed / total flight time - cannot be computed: no machine knows how
	// long the flight will last until it has ended, the ball can be caught, intercepted or bounced at
	// any point, and a client would have to be told the answer. Inventing a duration and ramping
	// against it would be a timer standing in for a state, which is the thing the Core's own
	// comments refuse to do twice over (see the rest/settle rules in TraceCore.cpp).
	//
	// So the peak is found from the arc's own geometry instead. A thrown Core is a ballistic body:
	// its vertical speed is largest at the launch and at the landing and passes through ZERO at the
	// apex. So
	//
	//     ApexWeight = horizontal speed / (horizontal speed + |vertical speed|)
	//
	// is 1 exactly at the apex and falls off towards both ends of the arc, is a STATELESS function of
	// LooseVelocity - which already replicates, so every machine computes the same number for free -
	// and cannot drift, double-advance on a hitch or need an edge to be detected. It is also honest
	// about the degenerate case: a flat rail-height throw has no apex, its weight sits near 1 for the
	// whole flight, and "all apex" is the correct answer for an arc with no rise in it.
	//
	// The trail is then ALSO clamped to the distance the ball has actually covered since this machine
	// saw the throw begin, so it grows out of the ball instead of appearing at full length on frame
	// one - geometry that springs into existence is the tell that reads as a bug rather than a trail.
	// That uses the same ArtStateStartTime edge, subtracted, for the same reason as the halo.
	// ---------------------------------------------------------------------------------------------
	bool bTrailVisible = false;
	// ThrownTrailBlend None means MakeGlowMID resolved NOTHING, and the library's own instruction for
	// that case is to hide the component: a bare /Engine/BasicShapes cylinder drawn at its default
	// material is a grey 100 uu tube, which is far worse than no trail at all.
	if (State == ETraceCoreArtState::Flight && ThrownTrailSegments.Num() > 0
		&& ThrownTrailBlend != ETraceFxBlend::None)
	{
		const FVector Velocity(LooseVelocity);
		const double Speed = Velocity.Size();
		if (Speed > 1.0)
		{
			const FVector Direction = Velocity / Speed;
			const double Horizontal = FVector(Velocity.X, Velocity.Y, 0.0).Size();
			const double Vertical = FMath::Abs(Velocity.Z);

			// The 1.0 uu/s guard is the same "is it actually moving" threshold ResolveCoreArtState
			// uses; below it the ratio is noise and 1.0 (treat it as the apex) is the stable answer.
			const float ApexWeight = (Horizontal + Vertical > 1.0)
				? static_cast<float>(Horizontal / (Horizontal + Vertical))
				: 1.f;

			const float ApexLengthUU = FMath::Max(0.f, CVarCoreThrownTrailLength.GetValueOnGameThread());
			const float WantedLength = ApexLengthUU
				* FMath::Lerp(TraceCoreArt::ThrownTrailMinLengthScale, 1.f, ApexWeight);

			const float Elapsed = FMath::Max(0.f, LocalTimeSeconds - ArtStateStartTime);
			const float Flown = static_cast<float>(Speed) * Elapsed;
			const float LengthUU = FMath::Min(WantedLength, Flown);

			// The head sits at the ball's BACK, not its centre: the doc says the trail "streams behind
			// the ball", and a trail starting at the heart would be drawn through the front half of a
			// mesh it is meant to be trailing. Half the DRAWN length, derived from the same constant
			// the mesh is scaled by, so a re-export moves both together.
			const FVector Head = GetActorLocation()
				- Direction * (TraceCoreArt::TargetLengthUU * 0.5);
			const FVector Tail = Head - Direction * static_cast<double>(LengthUU);

			if (LengthUU > 1.f)
			{
				// THE TAPER. Three stacked cylinders, each at the radius of its own mid-point along
				// the ideal cone, which is UTraceFxShapes' answer to "one mesh cannot taper" and is
				// shared with §2's beam so the two cannot be built differently.
				//
				// The raw pointers are copied onto the stack rather than handed over as the TArray's
				// own storage: that array holds TObjectPtr<>, whose in-memory layout is NOT a plain
				// pointer in every build configuration, and reinterpreting it as one would be a bug
				// that compiles cleanly and only shows up where it was never tested.
				UStaticMeshComponent* SegmentPtrs[TraceCoreArt::ThrownTrailSegmentCount] = {};
				const int32 SegmentCount = FMath::Min(ThrownTrailSegments.Num(),
					TraceCoreArt::ThrownTrailSegmentCount);
				for (int32 Index = 0; Index < SegmentCount; ++Index)
				{
					SegmentPtrs[Index] = ThrownTrailSegments[Index];
				}

				UTraceFxShapes::TaperBetween(MakeArrayView(SegmentPtrs, SegmentCount),
					Head, Tail,
					TraceCoreArt::ThrownTrailHeadRadiusUU, TraceCoreArt::ThrownTrailTailRadiusUU);

				const float Opacity = FMath::Lerp(TraceCoreArt::ThrownTrailMinOpacity,
					TraceCoreArt::ThrownTrailMaxOpacity, ApexWeight);
				if (!FMath::IsNearlyEqual(ArtTrailAppliedOpacity, Opacity, 0.004f))
				{
					for (UMaterialInstanceDynamic* SegmentMID : ThrownTrailMIDs)
					{
						// Same intensity/opacity argument as the halo: the additive parent has no Glow
						// scalar, so 1.0 and the weight on the opacity is the whole dynamic range there
						// is, and the ball's own cyan (3.4x) does the brightness in the emissive half.
						UTraceFxShapes::SetGlow(SegmentMID, ThrownTrailBlend, CyanTint,
							/*Intensity=*/1.f, Opacity);
					}
					ArtTrailAppliedOpacity = Opacity;
				}

				bTrailVisible = true;
			}
		}
	}

	for (UStaticMeshComponent* Segment : ThrownTrailSegments)
	{
		if (Segment != nullptr && Segment->IsVisible() != bTrailVisible)
		{
			Segment->SetVisibility(bTrailVisible);
		}
	}
	if (!bTrailVisible)
	{
		ArtTrailAppliedOpacity = -1.f;
	}
}

void ATraceCore::UpdateCoreArt()
{
	if (!bPackArtActive || PackMesh == nullptr)
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// THE A/B, LIVE. bPackArtActive says the art is AVAILABLE; this says whether it is being drawn, so
	// `Trace.Core.PackArt 0` swaps the new ball for the pre-v31 sphere with the match still running and
	// `1` swaps it back. Two visibility compares per tick, and a SetVisibility only on the edge.
	const bool bDrawPack = CVarCorePackArt.GetValueOnGameThread() != 0;
	if (PackMesh->IsVisible() != bDrawPack)
	{
		PackMesh->SetVisibility(bDrawPack);
		if (Mesh != nullptr)
		{
			Mesh->SetVisibility(!bDrawPack);
		}
	}
	if (!bDrawPack)
	{
		if (HeartLight != nullptr && HeartLight->IsVisible())
		{
			HeartLight->SetVisibility(false);
		}
		// SPEC v32 §3. The A/B switch turns the whole pack presentation off, and the FX geometry is
		// part of that presentation: a halo blooming around the pre-v31 engine sphere would be the
		// new effects photographed against the old ball, which is exactly what this switch exists to
		// let a reviewer avoid.
		HideCoreArtGeometry();
		return;
	}

	// THIS MACHINE'S OWN CLOCK, not the shared server clock. Nothing here has to agree between
	// machines - it is a pose and a glow - and the shared clock is only meaningful once a GameState
	// has replicated, which a Core drawn during travel cannot assume.
	const float Now = static_cast<float>(World->GetTimeSeconds());

	const ETraceCoreArtState State = ResolveCoreArtState();
	if (!bArtStateApplied || State != AppliedArtState)
	{
		AppliedArtState = State;
		bArtStateApplied = true;
		ArtStateStartTime = Now;
	}

	switch (State)
	{
	case ETraceCoreArtState::Carried:
	{
		// Component rotation IDENTITY so the clip's authored spin axis lines up with world up and the
		// turntable idle reads exactly as the artist made it, floating over the holder's head.
		PackMesh->SetRelativeRotation(FRotator::ZeroRotator);

		// Pickup, then Idle. The pack README's own instruction, and the switch is elapsed-since-the-
		// possession-edge rather than a query of the playhead - see the flare note above.
		const float PickupLength = (ArtPickupAnim != nullptr) ? ArtPickupAnim->GetPlayLength() : 0.f;
		const bool bStillCracking = (Now - ArtStateStartTime) < PickupLength;

		SetCoreArtAnim(bStillCracking ? ArtPickupAnim : ArtIdleAnim, /*bLooping=*/!bStillCracking, 1.f);
		break;
	}

	case ETraceCoreArtState::Flight:
	{
		// NOSE ALONG THE VELOCITY. MakeFromX maps the mesh's local +X - the nose socket, the throw
		// axis, the one axis the pack documents - onto the direction of travel, and re-derives it
		// every frame so the nose follows the arc down as gravity bends it. The ROLL is left to the
		// clip; MakeFromX's own choice of roll is irrelevant because the ball is rolling anyway.
		const FVector Velocity = LooseVelocity;
		if (!Velocity.IsNearlyZero(1.0))
		{
			PackMesh->SetRelativeRotation(FRotationMatrix::MakeFromX(Velocity).Rotator());
		}

		// The authored 8.00 rev/s scaled to the FX notes' number. RELATIVE to the clip's own rate, so
		// a re-export cannot silently change the spin.
		const float Wanted = FMath::Max(0.f, CVarCoreFlightSpin.GetValueOnGameThread());
		SetCoreArtAnim(ArtThrowAnim, /*bLooping=*/true, Wanted / TraceCoreArt::ThrowClipRevPerSecond);
		break;
	}

	case ETraceCoreArtState::Rest:
	default:
	{
		// STANDS UP ON ITS POINT. Pitch +90 takes the mesh's local +X to world +Z, so the long axis is
		// vertical and the ball is balanced on its rear cap with the nose at the sky. Yaw and roll are
		// zero: the TURN comes from the clip, about the same axis, so there is no hand-written motion
		// to fight it and nothing to keep in step.
		PackMesh->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));

		// See the ART block: A_Core_Idle's spin axis is horizontal once the ball is stood up, so the
		// clip that spins about the LONG axis is the one that can be used here, played at the turn
		// rate A_Core_Idle authors for the ground.
		const float Wanted = FMath::Max(0.f, CVarCoreRestSpin.GetValueOnGameThread());
		SetCoreArtAnim(ArtThrowAnim, /*bLooping=*/true, Wanted / TraceCoreArt::ThrowClipRevPerSecond);
		break;
	}
	}

	UpdateCoreArtEmissive(State, Now);

	// SPEC v32 §3, AND IT RUNS AFTER THE EMISSIVE ON PURPOSE: it reads back the team tint that call
	// just resolved (ArtAppliedTint) rather than deriving the team a second time, so the ball and its
	// trail can never be two different colours.
	UpdateCoreArtGeometry(State, Now);
}

