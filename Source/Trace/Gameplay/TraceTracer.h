#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "Gameplay/TraceFxShapes.h"                 // SPEC v32 §1 — the shared shape library
#include "Gameplay/TraceRailgunFireCurve.h"         // SPEC v32 §2 — the decay length, not retyped

#include "TraceTracer.generated.h"

class ATraceCharacter;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMeshComponent;
class UWorld;

/**
 * SPEC v30 §5 — WHERE THE BEAM'S NEAR END CAME FROM, AND HOW IT WAS DECIDED.
 *
 * One shot's worth of muzzle resolution, filled in by ATraceTracer::ResolveViewModelBeamStart().
 * It exists because the answer is now a CHOICE between two sources, and a choice that is not
 * reported is a choice nobody can check: Trace.Smg.Probe prints every field of this so that "the
 * beam leaves the SMG" can be established from numbers rather than from a squint at a screenshot.
 *
 * Plain struct, not a USTRUCT: nothing here is replicated, serialised or exposed to Blueprint, and
 * it is filled several times a second on the client that is shooting.
 */
struct FTraceTracerBeamStart
{
	/** False when nothing is drawn to leave from (third person, dead, dedicated server, no rig). */
	bool bValid = false;

	/** The point the beam actually starts at: world space, AFTER the first-person re-projection. */
	FVector Start = FVector::ZeroVector;

	/** The pawn's own muzzle marker, un-morphed and morphed. Both zero if it has none. */
	FVector MarkerRaw = FVector::ZeroVector;
	FVector MarkerDrawn = FVector::ZeroVector;
	bool bHasMarker = false;

	/** The gun ACTUALLY on screen, found by its mesh, and its own muzzle landmark carried to world. */
	FName GunMesh = NAME_None;
	FVector GunMuzzleLocalCm = FVector::ZeroVector;
	FVector GunMuzzleRaw = FVector::ZeroVector;
	bool bHasGun = false;

	/**
	 * Distance between the marker and the on-screen gun's own muzzle, in uu, or -1 when either is
	 * missing. This is the number that decides which source wins, so it is the number to look at
	 * first when the beam comes out of the wrong place.
	 */
	double MarkerToGunUU = -1.0;

	/** True when Start came from the gun's mesh landmark rather than from the pawn's marker. */
	bool bFromGunMesh = false;
};

/**
 * SPEC v32 §2 — ONE LIVE BEAM, MEASURED OFF ITS OWN COMPONENTS.
 *
 * Nearly everything a verifier is told to check — "the beam's resolved length, the taper radii in
 * uu, the halo opacity and the flash scale over time" — read BACK off the transforms that are
 * actually on screen rather than re-derived from the constants that were meant to produce them.
 *
 * That distinction is the whole point. A probe that recomputes `3.0` from the same constant the
 * effect used proves only that one number is spelled the same way twice; this project has three
 * harnesses on record that printed a pass over their own failure by doing exactly that, and this
 * one made it four before a verifier caught it. Every `Measured*` field below comes out of
 * UTraceFxShapes::RadiusUUFromShapeScale() applied to a live component scale, so if the taper is
 * silently 100x wrong the probe says 300.0 and not 3.0.
 *
 * "NEARLY" IS LOAD-BEARING, and the exceptions are named where they live: HaloOpacityNow is a
 * recomputation because a MID cannot be read back, and FlashScaleNow is a quotient of a measurement
 * by a constant and must never be used as a denominator. Trace.Fx.Beam prints the provenance of
 * every row of its verdict so a reader never has to take this paragraph on trust.
 *
 * Plain struct, dev-only in practice, nothing replicated.
 */
struct FTraceTracerShotDebug
{
	/** Which of the two profiles this shot was built with, and how that was decided. */
	bool bSmgProfile = false;
	bool bFirstPersonShot = false;
	FName GunMesh = NAME_None;

	/** What the FX doc asks for, in uu, after the settings knobs have had their say. */
	float ProfileMuzzleRadiusUU = 0.f;
	float ProfileTipRadiusUU = 0.f;
	float ProfileHaloRadiusUU = 0.f;
	float ProfileHaloOpacity = 0.f;
	float ProfileConeRadiusUU = 0.f;
	float ProfileConeHeightUU = 0.f;

	/** MEASURED off the live components. */
	float AgeSeconds = 0.f;

	/**
	 * The age UpdateEffect was LAST CALLED WITH, i.e. the point on the authored curves that the
	 * transforms below were laid at. Recorded by the effect, not inferred by the probe.
	 *
	 * This exists so the muzzle-cone check can be a real one. The cone is the only piece of this
	 * effect whose SIZE animates — 0.55x to 3.2x in 0.28 s, which is 2.5 uu of radius per 60 Hz
	 * frame — so grading its measured radius against the doc's curve needs the curve evaluated at
	 * exactly the age the geometry was written at, not at whatever the clock says when the probe
	 * happens to look. AgeSeconds and GeometryAgeSeconds are the same number today (the probe
	 * samples from the core ticker, outside UWorld::Tick, so it cannot land between the world
	 * clock advancing and the actor tick that reads it) and the probe PRINTS BOTH so that if that
	 * ever stops being true the log says so instead of the check going quietly, wrongly red.
	 */
	float GeometryAgeSeconds = 0.f;
	float LengthUU = 0.f;
	float MeasuredMuzzleRadiusUU = 0.f;
	float MeasuredTipRadiusUU = 0.f;
	float MeasuredSegmentRadiiUU[3] = { 0.f, 0.f, 0.f };
	int32 SegmentCount = 0;
	float MeasuredHaloRadiusUU = 0.f;
	float MeasuredConeRadiusUU = 0.f;
	float MeasuredConeHeightUU = 0.f;

	/**
	 * The instantaneous animated quantities. NEITHER OF THESE IS A READBACK, and the distinction
	 * matters enough that Trace.Fx.Beam now prints it in the verdict table beside every row.
	 *
	 * HaloOpacityNow is RECOMPUTED from the same fade expression UpdateEffect uses, because a
	 * material instance will not hand back a scalar that was written into it — there is no
	 * measurement to take. It is still worth checking (a fade that never starts is a real bug) but
	 * it cannot catch a wrong opacity CONSTANT, and it is labelled "recomputed" for that reason.
	 *
	 * FlashScaleNow is DERIVED FROM a readback — MeasuredConeRadiusUU divided by the authored
	 * radius — which makes it a fine thing to log and a catastrophic thing to grade against.
	 * Dividing the measurement back by it recovers the authored constant identically, whatever the
	 * cone's transform actually is; the verdict did exactly that for two of six checks and passed a
	 * 100x metres/centimetres error. Grade MeasuredConeRadiusUU against the doc's curve evaluated at
	 * GeometryAgeSeconds instead.
	 */
	float HaloOpacityNow = 0.f;
	float FlashScaleNow = 0.f;
	bool bHaloVisible = false;
	bool bFlashVisible = false;

	/** Which blend each piece ended up in, after UTraceFxShapes' degradation ladder. */
	ETraceFxBlend CoreBlend = ETraceFxBlend::None;
	ETraceFxBlend HaloBlend = ETraceFxBlend::None;
	ETraceFxBlend FlashBlend = ETraceFxBlend::None;
	bool bHaloTwoSided = false;
};

/**
 * The railgun shot effect. Purely cosmetic, one actor per shot, self-deleting.
 *
 * --- WHAT IT IS MADE OF, AND WHY --------------------------------------------------------------
 *
 * The brief was "the hitscan projectiles look really odd, make them a nice railgun animation".
 * What was here before was a single 3 uu cylinder at a flat colour that lived 0.08 s: a grey-blue
 * stick, no muzzle, no impact, no falloff. It read as debug geometry because it WAS debug
 * geometry.
 *
 * A railgun shot has three beats, and this actor draws all three out of engine primitives, because
 * Niagara would need .uasset VFX this project cannot author (build contract 2):
 *
 *   1. CORE      a thin, almost white TAPERED beam from muzzle to impact. Near-white rather than
 *                team coloured on purpose - a real energy weapon's centre is hotter than its edges,
 *                and a white core with a coloured halo is what makes it read as *bright* instead of
 *                merely *blue*. It is emissive far past 1.0, so the scene's bloom does the wide
 *                soft halo for free; that is the same trick every neon surface in this arena uses.
 *   2. HALO      a fatter, dimmer, fully team-coloured sleeve around the core, drawn additively so
 *                it cannot occlude the core. Optional: if the additive engine material is
 *                unavailable the effect degrades to core-plus-bloom and still looks deliberate.
 *   3. MUZZLE    a short, bright CONE at the near end of the beam, pointing down it. Suppressible
 *                from settings (UTraceSettings::bTracerMuzzleFlash).
 *
 * --- SPEC v32 §2 RE-PROFILED ALL THREE AGAINST Art/Pack/docs/unreal-fx_README.md ---------------
 *
 * The artist's recipe gives every one of these a NUMBER, and until this pass none of them was on
 * screen: a v31 verifier grepped the tree and reported "EVERY PIECE OF FX GEOMETRY IN
 * unreal-fx_README IS ABSENT". The structure below is the one that was already here - it was right,
 * it solved the hard first-person problem, and it is re-profiled rather than replaced. What changed:
 *
 *   * THE CORE TAPERS. r 3.0 -> 1.3 uu (pistol), 2.4 -> 1.3 uu (SMG), which is the doc's
 *     0.030 -> 0.013 m and 0.024 m in Unreal's centimetres. One mesh cannot taper, so it is three
 *     stacked cylinders - see UTraceFxShapes::TaperAlongLocalZ for why stacked segments and not a
 *     cone.
 *   * THE SHEATH BECAME THE HALO. The doc asks for a double-sided translucent sleeve at r 5.2 uu
 *     and ~55% opacity. There was already an additive sheath at a different radius, so the two are
 *     RECONCILED INTO ONE SLEEVE rather than a third cylinder being stacked inside them - which is
 *     what §2 asks for in as many words, and is also the only version that does not triple the
 *     overdraw down the middle of the screen.
 *   * THE MUZZLE SPHERE BECAME A CONE, r 16 uu / h 30 uu (pistol) / r 13 uu (SMG), pointing down
 *     the beam, for the first 0.28 s, scaling 0.55 -> 3.2x.
 *   * THE LIFE IS NOW HOLD-THEN-FADE: 0.10 s at full, then a decay whose length is
 *     TraceRailgunFireCurve's own tail rather than a second copy of "0.75".
 *
 * --- LENGTH: THE ONE PLACE THIS DELIBERATELY DEPARTS FROM THE FX DOC ---------------------------
 *
 * The doc says the beam is "~2.6 m" long. IT IS NOT, HERE, AND IT MUST NOT BE. That number is an
 * artefact of the preview viewer the recipe was written against: a turntable with no world in it,
 * where a beam has nothing to end on, so the artist gave it an arbitrary length so it would be
 * visible in the frame. In a hitscan shooter the beam's length is the ONLY thing it says - it runs
 * MUZZLE -> IMPACT and terminating exactly on the hit point is the entire information content of a
 * tracer. Spec v4 §4 deleted the impact sphere for precisely this reason ("so it's just a bullet
 * trace, which makes it easier to see where your shots are going"), and a fixed 260 uu stub would
 * put that information back behind a wall.
 *
 * So the length is the shot's own, and THE TAPER RUNS OVER THAT ACTUAL LENGTH, not over 260 uu. The
 * doc's radii are honoured exactly; only its length is adapted. This is a stated deviation, not an
 * oversight, and Trace.Fx.Beam prints the resolved length on every sample so it can be checked.
 *
 * --- THE FOURTH BEAT, AND WHY IT IS GONE ------------------------------------------------------
 *
 * There used to be an IMPACT beat: a sphere at the hit point that expanded from 10 to 52 uu as it
 * dimmed - "the pop that gives the shot its weight". Spec v4 §4 deletes it, verbatim: "Remove the
 * sphere from the end of the bullet tracer hitscan animation, so it's just a bullet trace, which
 * makes it easier to see where your shots are going."
 *
 * That is the right call and the old comment above contained the reason without noticing it. The
 * sphere's whole job was to sit ON the point the shooter is trying to read, and at 52 uu of unlit
 * emissive under this arena's bloom it did not merely mark that point, it covered it. A player
 * checking where their shots are landing was being shown a ball of light exactly where the answer
 * was. The beam already terminates at the impact, so the information was never lost with it.
 *
 * DELETED, NOT DISABLED. There is no bImpactFlash setting, because a knob to restore it would be a
 * knob to restore the reported problem.
 *
 * The whole thing snaps to full length on frame one (it is hitscan - there is no travel time to
 * animate) and then decays over TracerLifeSeconds with an ease-out: bright and instant, then a
 * short weighty fade. Nothing translates; only intensity and thickness animate.
 *
 * --- WIDTH ------------------------------------------------------------------------------------
 *
 * Spec v4 §4 also asks for a thinner beam, with the radius tunable. The radius, its two clamps, the
 * halo ratio and the muzzle flash are all UTraceSettings properties (Category "Tracer") read fresh
 * on every shot, so the look retunes with PIE running. The proportional-to-length model is kept and
 * scaled down rather than replaced with a constant - see the comment on that block in
 * TraceSettings.h for why a constant radius cannot work across a 24000+ uu arena.
 *
 * --- THE FIRST PERSON START, AND WHY IT IS ASKED FOR RATHER THAN GUESSED ----------------------
 *
 * ATraceCharacter::GetMuzzleLocation() sits 22 uu in front of the eye, ON the aim ray, and
 * GetAimDirection() is that same ray. So to the shooter's own camera the beam is a line pointing
 * directly away from the viewer: it projects to a single point behind the crosshair and is, in the
 * strict geometric sense, invisible. That is a large part of why the old tracer "looked odd" - in
 * first person there was nothing to see but the impact. The near end therefore has to be moved to
 * where the gun VISIBLY is, for the local shooter only.
 *
 * IT USED TO BE MOVED BY HAND, AND SPEC v26 §4 IS THE BILL FOR THAT. Three constants lived here -
 * FirstPersonStandoffUU 120, FirstPersonRightOffsetUU 42, FirstPersonDownOffsetUU 17 - which pushed
 * the start down the shot and then across into "the viewmodel's corner of the screen". They were
 * eyeballed against the small procedural cube gun. When the 185 cm railgun replaced it the gun moved
 * and the constants did not, and the beam started roughly 140 px too far right and 60 px too high:
 * "above or behind it", verbatim, which is the report.
 *
 * THE START IS NOW DERIVED FROM THE BARREL. ATraceCharacter::GetViewModelMuzzleViewPoint() returns
 * the world point at which the viewmodel's muzzle marker - a scene component parented to the railgun
 * body at the mesh's own (107.4, 0, 4.5) cm muzzle landmark - is actually DRAWN, having been put
 * through the same first-person re-projection the GPU applies to it. So the beam leaves the barrel by
 * construction: it tracks the recoil, the sway, the walk bob and the slide dip, it is correct for the
 * fallback cube gun as well as for the railgun, and it follows the player's own field-of-view slider.
 * There is no number here to re-tune when the gun moves again.
 *
 * --- AND NOW THERE ARE TWO GUNS (spec v30 §5) -------------------------------------------------
 *
 * "The beam must leave whichever gun is actually on screen." Demo 24 added the SMG and Demo 25 gave
 * it the 3 key, so from this pass onward the pawn can be holding either weapon - and a single muzzle
 * marker is a single answer to a question that now has two. If the marker is left parented to the
 * pistol while the SMG is drawn, the beam starts at the pistol's landmark: 14.58 uu of rig space away
 * from the SMG's own aperture, MEASURED on the shipped rig by Trace.Smg.Probe, which is the same class
 * of error the three deleted constants caused, just smaller.
 *
 * SO THE GUN ON SCREEN IS ASKED DIRECTLY, AND THE MARKER IS ONLY TRUSTED WHEN IT AGREES WITH IT.
 * ResolveViewModelBeamStart() finds the weapon body the pawn is actually DRAWING (a visible static
 * mesh component whose asset is one of the known viewmodel guns), carries that mesh's own measured
 * muzzle landmark through the component's live world transform, and compares the result with the
 * pawn's marker:
 *
 *   * they agree (within MuzzleAgreementUU)  ->  the pawn's own answer is used, byte for byte the
 *                                                v26 path. This is the pistol today, and it is also
 *                                                the SMG the moment the marker is parented to it.
 *   * they disagree                          ->  the gun on screen wins, because it is the thing the
 *                                                player can see the beam failing to leave.
 *   * no known gun is drawn (cube gun,       ->  the pawn's marker, exactly as before.
 *     no art, third person)
 *
 * THIS IS A SAFETY NET, NOT A SECOND PLACEMENT SYSTEM. The mesh-local landmarks it carries are
 * measured properties of the art (the SMG's aperture centre was measured from the GLB and confirms
 * the spec's -0.588 m), not tuning values, and there is nothing here to re-tune when the rig moves -
 * the transform is read live off the component. The moment ATraceCharacter parents its marker to the
 * active gun, every one of these lookups agrees with it and this code stops changing any answer;
 * Trace.Smg.Probe prints MarkerToGunUU so that "it stopped changing the answer" is a measurement
 * rather than a hope.
 *
 * Nothing about the gameplay ray changes. The shot ORIGIN is camera-derived on purpose - it is what
 * makes the crosshair honest, it is evaluated on the server too, and it is not this actor's business.
 * This is the near end of a cosmetic beam, on one machine.
 *
 * The old standoff was also doing a SAFETY job, and that survives the change for free. An unlit
 * emissive sphere 22 uu from the eye subtends about 30 degrees of screen, and unlit emissive is
 * distance-invariant, so it arrives at full intensity however close it is - the recipe for a
 * point-blank whiteout. The muzzle marker sits ~85 uu out, a couple of uu further than the 120 uu
 * standoff put the start once its lateral offset is counted, so the flash and the sheath subtend no
 * more of the frame than they did before.
 *
 * --- NETWORKING -------------------------------------------------------------------------------
 *
 * Never replicated. Every machine spawns its own:
 *  - the shooter spawns one the instant it pulls the trigger (predicted, zero latency),
 *  - everyone else spawns one from UTraceWeaponComponent::MulticastFireEffects, which deliberately
 *    skips the shooter so a predicted beam is never drawn twice.
 *
 * EXACTLY ONE of these actors exists per shot. Debug/TraceInputHarness.cpp counts live
 * ATraceTracer actors to count shots emitted; splitting the effect into several actors would
 * silently inflate that measurement. Hence four COMPONENTS on one actor rather than four actors.
 */
UCLASS()
class TRACE_API ATraceTracer : public AActor
{
	GENERATED_BODY()

public:
	ATraceTracer();

	virtual void Tick(float DeltaSeconds) override;

	/**
	 * Spawns and configures one railgun shot.
	 *
	 * @param From      Muzzle position.
	 * @param To        Impact position (or the far end of the ray if nothing was hit).
	 * @param Color     Shooter's team colour.
	 * @param bImpacted RETAINED AND NOW UNUSED. It used to gate the impact pop, which spec v4 §4
	 *                  deleted. The parameter is kept, with its default, so that every existing call
	 *                  site (UTraceWeaponComponent's predicted and multicast paths, the input
	 *                  harness) keeps compiling untouched — removing it would be a change to files
	 *                  this slice does not own, for no behavioural gain. If a future beat ever needs
	 *                  to know whether the shot connected, it is already plumbed.
	 *
	 * Returns nullptr on a dedicated server (no visuals), for a degenerate/non-finite segment, or
	 * when the world is invalid. Callers may ignore the return value.
	 */
	static ATraceTracer* Spawn(UWorld* World, const FVector& From, const FVector& To, const FLinearColor& Color, bool bImpacted = true);

	/**
	 * SPEC v30 §5 — where a beam fired by @p Shooter would leave the gun that is ON SCREEN.
	 *
	 * The whole of the first-person start decision, in one function, so that the beam and every probe
	 * that checks the beam are reading the SAME code. A verifier that re-implements the thing it is
	 * verifying can only ever prove that two implementations agree.
	 *
	 * Cosmetic and local-only. It answers false whenever there is nothing drawn to leave from, and
	 * callers must then use the true shot origin, exactly as they did before this existed.
	 */
	static bool ResolveViewModelBeamStart(const ATraceCharacter* Shooter, FTraceTracerBeamStart& OutInfo);

	/**
	 * The same answer for whoever the local viewer is looking out of. For Trace.Smg.Probe: it prints
	 * where the next beam WOULD start without anybody having to pull a trigger first.
	 */
	static bool DescribeLocalBeamStart(const UWorld* World, FTraceTracerBeamStart& OutInfo);

	/**
	 * The mesh-local muzzle landmark of a known viewmodel gun mesh, in the mesh's own centimetres.
	 *
	 * @param StaticMeshName e.g. "SM_RailgunSmg_Body". Returns false for anything not in the table,
	 *                       which is the honest answer for the procedural cube gun and for any art
	 *                       that arrives after this file was written.
	 */
	static bool GetGunMuzzleLandmark(FName StaticMeshName, FVector& OutMeshLocalCm);

	/**
	 * SPEC v32 §2 — this beam, as it is RIGHT NOW, measured off its own components.
	 *
	 * For Trace.Fx.Beam. Returns false only when the shot never got as far as being drawn (a
	 * degenerate segment). See FTraceTracerShotDebug for why the numbers are read back rather than
	 * recomputed.
	 */
	bool DescribeShot(FTraceTracerShotDebug& OutInfo) const;

	/**
	 * The newest live tracer in @p World, or null. Trace.Fx.Beam follows the most recent shot.
	 *
	 * @param bFirstPersonOnly  restrict the search to beams that were placed on the LOCAL VIEWER'S
	 *                          OWN VIEWMODEL, i.e. bFirstPersonShot. THE PROBE PASSES TRUE AND HERE
	 *                          IS WHY: a match has bots in it, every one of them spawns a tracer for
	 *                          every shot it takes, and "the newest tracer in the world" is very
	 *                          often one of theirs. A remote beam is not told who fired it (see the
	 *                          profile note in InitTracer), so it correctly draws the PISTOL profile
	 *                          whatever the shooter is holding — and Trace.Fx.BeamSmg was measuring
	 *                          exactly that and reporting the SMG's beam as a pistol's. Worse, the
	 *                          pistol arm was PASSING on somebody else's beam for the same reason,
	 *                          which is a green that proves nothing about the gun in your hands.
	 *                          False keeps the old "newest of anybody's" behaviour.
	 */
	static ATraceTracer* GetNewestTracer(const UWorld* World, bool bFirstPersonOnly = false);

	/**
	 * *** THE ONE SIZE KNOB: Trace.Fx.BeamScale, read live. ***
	 *
	 * A single multiplier applied to EVERY radius and length in the beam's authored profile — core
	 * muzzle, core tip, halo, and both dimensions of the muzzle-flash cone — at the very end of
	 * InitTracer's width block, after the legibility floor and after the three shipped UTraceSettings
	 * knobs have had their say. Because it multiplies all of them by the same number, every
	 * proportion the FX doc authored (tip/muzzle 0.433, halo/muzzle 1.733, cone 16:30) survives
	 * untouched and only the absolute size changes. That is the whole design: the doc's shape, at a
	 * different size.
	 *
	 * WHY IT EXISTS. Photographed in Saved/Screenshots/v31integ_42_pistol_firing.png: at 1.0 the shot
	 * reads as a solid white plank about as wide as the arena's structural rails — architecture, not
	 * light. The near end of a first-person beam is ~85 uu from the eye, where the halo's 5.2 uu
	 * radius subtends roughly 100 px of a 1600 px frame before bloom widens it further, and the
	 * legibility floor makes a long shot FATTER still (up to 6.5 uu, i.e. an 11.3 uu halo).
	 *
	 * WHY A CVAR RATHER THAN A UTraceSettings PROPERTY. This file's neighbours already made that
	 * call for exactly this kind of number — Trace.Core.FxGeometry, Trace.Core.ThrownTrailLengthUU
	 * and the two heart-light knobs are all CVars. The house rule about UPROPERTY(config) is about
	 * GAMEPLAY tunables a designer ships; this is the size of a piece of decoration, and being a CVar
	 * is what lets the next tuning pass photograph three values without a rebuild.
	 *
	 * Public and an accessor rather than a raw CVar read because Trace.Fx.Beam's verdict has to grade
	 * doc x knob on its three ABSOLUTE rows, and a harness with its own copy of the knob is a second
	 * place for it to live. Note what the split does and does not buy: the knob is an INPUT to the
	 * effect, exactly like TracerSheathRadiusRatio, so multiplying the expected side by it is honest;
	 * the two RATIO rows are scale-invariant and keep grading the doc's raw proportions, so they can
	 * still tell a pistol from an SMG and Trace.Fx.BeamRedArm stays red, at any knob value.
	 *
	 * Clamped to 0.05..4.0 on the way out, so no console typo can make the beam invisible (which
	 * looks exactly like "the tracer stopped rendering") or a wall across the arena.
	 */
	static float GetBeamProfileScale();

	/**
	 * How many stacked cylinders the tapered core is made of.
	 *
	 * Public because the probe sizes its own array from it, and because a verifier reading the log
	 * should be able to find out from the header how many radii it is being shown. THREE: see
	 * UTraceFxShapes::TaperAlongLocalZ for the arithmetic on why the resulting step is not visible.
	 */
	static constexpr int32 BeamTaperSegments = 3;

	// =============================================================================================
	// TIMING — public because the probe reports the window it sampled
	//
	// Trace.Fx.Beam has to say WHICH window its numbers were taken from, and a probe that carries its
	// own copy of "the hold is 0.10 s" is a second place for that fact to live. These are the effect's
	// timing contract, so they are stated once, here, and read by anything that needs them.
	// =============================================================================================

	/** Doc: "Spawn at discharge, hold 0.10 s, fade over the decay." */
	static constexpr float BeamHoldSeconds = 0.10f;

	/**
	 * THE DECAY, NOT RETYPED. SPEC v32 §2: "The pistol's decay is 0.75 s (already a constant
	 * somewhere near the discharge driver — reuse it, do not retype 0.75)."
	 *
	 * It is the tail of the authored fire clip: everything from the end of the flash
	 * (DecayStartSeconds, 1.15) to the end of the clip (ClipSeconds, 1.90). Derived rather than
	 * copied so that a re-export of Art/Railgun/fire_curves.json — which regenerates
	 * TraceRailgunFireCurve.h — moves the beam's fade and the weapon's emissive fade together. Two
	 * copies of one number is how they end up disagreeing.
	 */
	static constexpr float BeamDecaySeconds =
		TraceRailgunFireCurve::ClipSeconds - TraceRailgunFireCurve::DecayStartSeconds;

	/**
	 * Total life: hold plus decay. 0.85 s, up from the 0.17 s this effect used before v32.
	 *
	 * That is a FIVE-FOLD increase in how long a tracer actor lives, and it is worth naming what it
	 * costs: at 600 RPM a held SMG trigger now keeps ~8.5 of these alive at once per shooter instead
	 * of ~1.7. They are five collisionless, shadowless, unlit components each and they still expire
	 * on their own InitialLifeSpan, so the count is bounded by (rate x life) and cannot grow — but it
	 * is a real change and the two harnesses that COUNT live ATraceTracer actors (the input harness's
	 * shots-emitted set, Trace.Smg.Verify's before/after delta) both get MORE reliable from it, not
	 * less, because a longer-lived actor is harder to sample between.
	 */
	static constexpr float TracerLifeSeconds = BeamHoldSeconds + BeamDecaySeconds;

	/**
	 * Doc: the muzzle cone is "visible for the first 0.28 s of decay, scaling 0.55 -> 3.2x".
	 *
	 * *** AND THE SMG FIRES EVERY 0.1 s, SO THREE OF THESE OVERLAP. ***
	 *
	 * DECIDED: for the shot the local player is looking down — the one on the viewmodel, a hand's
	 * breadth from the camera — there is exactly ONE cone in the world at a time and each shot
	 * RESTARTS it. A new first-person tracer retires the previous one's flash before lighting its
	 * own (see RetireMuzzleFlash and the GFirstPersonFlashOwner note in the .cpp).
	 *
	 * Two reasons, and the second is the one that matters. First, it is what a real muzzle does: a
	 * gun has one muzzle, and three superimposed flashes is not a brighter flash, it is a bug.
	 * Second, these cones are UNLIT AND ADDITIVE, which does not attenuate with distance and which
	 * sums literally rather than figuratively, and they grow to 3.2x of a 16 uu radius roughly 85 uu
	 * from the eye. One of those is a muzzle flash. Three summed is a flashbang — the exact failure
	 * the TracerMuzzleRadiusUU tooltip records having already been fixed once.
	 *
	 * REMOTE beams keep a flash each, deliberately. They are somebody else's gun tens of metres away,
	 * where overlapping flashes are both correct (two players CAN fire at once) and free, and where
	 * sharing one cone between shooters would mean one player's shot putting out another's flash.
	 *
	 * Neither arm can leak: the cone belongs to the tracer actor and the tracer actor expires on
	 * InitialLifeSpan whether or not anybody ever retires anything.
	 */
	static constexpr float MuzzleFlashSeconds = 0.28f;
	static constexpr float MuzzleFlashStartScale = 0.55f;
	static constexpr float MuzzleFlashEndScale = 3.2f;

protected:
	/** Positioned at the beam start and rotated so local +Z runs down the shot. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<USceneComponent> EffectRoot;

	/**
	 * Beat 1: the hot near-white centre of the beam, TAPERED (SPEC v32 §2).
	 *
	 * THREE COMPONENTS, ONE BEAM. A static mesh has one scale, so a cylinder cannot narrow along its
	 * own length; the taper is three stacked cylinders at decreasing radii, each at the radius of its
	 * own mid-point along the taper. UTraceFxShapes::TaperAlongLocalZ owns that arithmetic and its
	 * comment carries the argument for stacked segments over a clipped cone.
	 *
	 * They are still ONE ACTOR, which matters: Debug/TraceInputHarness.cpp counts live ATraceTracer
	 * actors to count shots emitted, so splitting the beam into several actors would silently inflate
	 * a measurement two other harnesses depend on. Hence five components on one actor, as before.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<UStaticMeshComponent> BeamSegments[3];

	/**
	 * Beat 2: the team-coloured sleeve around the core. r 5.2 uu, ~55% opacity (FX doc r 0.052 m).
	 *
	 * THIS IS THE OLD SHEATH, RE-PROFILED — not a third cylinder added inside it. SPEC v32 §2 asks
	 * for exactly that reconciliation, and the reason is overdraw: the halo runs the whole length of
	 * the beam, down the middle of the screen, up to eighty times a second, and a third full-length
	 * translucent tube would be the most expensive thing in the effect for a difference nobody could
	 * name. Hidden entirely if no non-occluding material resolved.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<UStaticMeshComponent> BeamHalo;

	/**
	 * Beat 3: muzzle flash — a CONE now, not a sphere, pointing down the beam. Gated on
	 * UTraceSettings::bTracerMuzzleFlash, which is a shipped setting players use.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<UStaticMeshComponent> MuzzleFlash;

	// ImpactFlash DELETED (spec v4 §4). See "THE FOURTH BEAT, AND WHY IT IS GONE" in the class
	// comment. The component, its MID, its two diameter constants, its intensity, its visibility flag
	// and the camera-proximity dimming that existed solely to stop it whiting out the screen at
	// point-blank range are all gone with it.

	// THE THREE MATERIAL UPROPERTYs MOVED TO UTraceFxShapes (SPEC v32 §1). M_TraceNeon, the engine's
	// additive material and the BasicShapeMaterial fallback are now found once, on that library's
	// CDO, with the same constructor-time FObjectFinders that used to be here — so the cooker still
	// follows them and every effect in the pass degrades through the same ladder instead of each one
	// re-implementing the fallback rule slightly differently.

private:
	void InitTracer(const FVector& From, const FVector& To, const FLinearColor& Color, bool bImpacted);

	/**
	 * Applies the whole look for an absolute age in seconds.
	 *
	 * SECONDS, NOT A NORMALISED ALPHA, and driven from the world clock rather than an accumulator —
	 * see the note on SpawnTimeSeconds. Half the numbers in this effect are 0.10 / 0.28 / 0.75 s and
	 * they have to mean those things, not "some fraction of a life that changed the last time
	 * somebody retuned the decay".
	 */
	void UpdateEffect(float AgeSeconds);

	/**
	 * Hides the muzzle cone for good. Called when its 0.28 s window ends — and by the NEXT shot,
	 * when this tracer is the one holding the shared first-person flash. See the comment on
	 * MuzzleFlashSeconds for why a held SMG trigger must not stack three cones on the viewmodel.
	 */
	void RetireMuzzleFlash();

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CoreMID;

	/**
	 * ONE MID, THREE SEGMENTS. The tapered core's three cylinders share a single dynamic material
	 * instance — created on the first segment and assigned to the other two — so they cannot
	 * possibly disagree about colour or brightness, and the per-frame fade is one parameter write
	 * instead of three. Two objects agreeing about one fact is a failure this codebase logs by name;
	 * the cheapest way not to have it is not to have two objects.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> HaloMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MuzzleMID;

	/** Team colour of the shot, cached for the per-frame fade. */
	FLinearColor ShotColor = FLinearColor::White;

	/** Near-white version of ShotColor used by the core and the muzzle. */
	FLinearColor HotColor = FLinearColor::White;

	/**
	 * The world time this shot was drawn at. AN ABSOLUTE CLOCK, NOT AN ACCUMULATOR.
	 *
	 * House rule, and it has bitten this project twice: a per-frame reader of a short quantity that
	 * adds DeltaSeconds to a counter double-advances on a hitch, drifts, and disagrees between
	 * machines. Every duration in this effect (0.10 s hold, 0.28 s flash, 0.75 s decay) is short
	 * enough for that to be visible. Age is therefore a pure function of World->GetTimeSeconds(),
	 * evaluated fresh, and there is no state to sample before or after advancing because there is no
	 * advancing.
	 */
	double SpawnTimeSeconds = 0.0;

	/**
	 * THE PROFILE FOR THIS SHOT, in uu, resolved once by InitTracer.
	 *
	 * RADII, not diameters. The FX doc speaks in radii ("r 0.030 -> 0.013"), spec v4 §4 speaks in
	 * radii, and UTraceSettings exposes radii; the old members here were diameters purely so the
	 * halving into a mesh scale was pre-done, which UTraceFxShapes::ShapeScaleForRadiusUU now owns.
	 * Carrying one unit end to end is worth more than saving a multiply.
	 *
	 * The initialisers are the pistol's authored profile so that a tracer which returned early from
	 * InitTracer (a degenerate segment, never drawn) holds documented values rather than garbage.
	 */
	float BeamLengthUU = 0.f;
	float CoreMuzzleRadiusUU = 3.0f;
	float CoreTipRadiusUU = 1.3f;
	float HaloRadiusUU = 5.2f;
	float HaloOpacityValue = 0.55f;
	float FlashConeRadiusUU = 16.0f;
	float FlashConeHeightUU = 30.0f;

	/** Which of the two weapon profiles was used, and how it was decided. For the probe. */
	bool bSmgProfile = false;
	bool bFirstPersonShot = false;
	FName ProfileGunMesh = NAME_None;

	/** The blend each piece actually got, after UTraceFxShapes' degradation ladder. */
	ETraceFxBlend CoreBlend = ETraceFxBlend::None;
	ETraceFxBlend HaloBlend = ETraceFxBlend::None;
	ETraceFxBlend FlashBlend = ETraceFxBlend::None;

	/**
	 * The age the geometry was last laid at. Written by UpdateEffect, read only by DescribeShot.
	 *
	 * NOT a clock and not an accumulator — it is a record of the argument UpdateEffect was handed,
	 * which is itself a stateless function of the absolute world clock (see SpawnTimeSeconds). The
	 * probe needs it because the muzzle cone's authored size is a function of age, so "is the live
	 * cone the right size?" is only a well-posed question once you know which age to ask it at.
	 */
	float GeometryAgeSeconds = 0.f;

	/** Set false once the muzzle flash has been retired, so it is only hidden once. */
	bool bMuzzleVisible = false;

	/** False when no non-occluding material resolved and the halo was dropped entirely. */
	bool bHaloVisible = false;

	// --- Tuning ---------------------------------------------------------------------------------
	//
	// What is left here is compile-time on purpose: these are the *look* of one effect, they are
	// meaningless outside this file, and every one of them was chosen against the arena's bloom
	// settings.
	//
	// THE WIDTHS ARE NO LONGER AMONG THEM. Spec v4 §4 asks for the beam radius to be tunable, and
	// this project's standing rule is that a new tunable is a UTraceSettings property - categorised,
	// clamped, tooltipped and live in PIE - because that panel is where the user tunes now. So
	// CoreDiameterPerLengthUU / Min / Max, SheathWidthRatio and MuzzleDiameterUU have MOVED to
	// UTraceSettings (Category "Tracer") as radii, and ImpactStartDiameterUU / ImpactEndDiameterUU /
	// ImpactIntensity are simply gone with the sphere they described.

	// =============================================================================================
	// SPEC v32 §2 — THE AUTHORED PROFILE
	//
	// Straight out of Art/Pack/docs/unreal-fx_README.md, converted ONCE, here, at the boundary:
	// THE FX DOC IS IN METRES AND UNREAL IS IN CENTIMETRES, so its 0.030 is 3.0 uu. Everything below
	// is in uu and nothing downstream divides or multiplies by 100 again — the only other conversion
	// in this effect is uu -> BasicShape scale, which lives in UTraceFxShapes and nowhere else.
	// =============================================================================================

	/** Doc: pistol beam "r 0.030 -> 0.013"; SMG "scaled to r 0.024". The tip is shared. */
	static constexpr float PistolCoreMuzzleRadiusUU = 3.0f;
	static constexpr float SmgCoreMuzzleRadiusUU = 2.4f;
	static constexpr float CoreTipRadiusProfileUU = 1.3f;

	/** Doc: "a wider translucent halo (r 0.052, double-sided, ~55% opacity)". */
	static constexpr float HaloRadiusProfileUU = 5.2f;
	static constexpr float HaloOpacityProfile = 0.55f;

	/** Doc: "a 6-sided cone (r 0.16, h 0.30) at the muzzle"; SMG "cone r 0.13". */
	static constexpr float PistolFlashConeRadiusUU = 16.0f;
	static constexpr float SmgFlashConeRadiusUU = 13.0f;
	static constexpr float FlashConeHeightProfileUU = 30.0f;

	/**
	 * WHAT THE THREE SHIPPED WIDTH KNOBS MEAN NOW, AND WHY THEY WERE NOT SIMPLY IGNORED.
	 *
	 * UTraceSettings' Tracer block is a shipped, tooltipped, player-facing panel, and spec v4 §4 put
	 * it there on purpose. The FX doc, meanwhile, gives absolute radii. Those two are only in tension
	 * if the knobs are read as absolutes, so they are read as SCALES ON THE AUTHORED PROFILE,
	 * normalised against the defaults the profile was authored alongside:
	 *
	 *     halo radius  = 5.2 uu * (TracerSheathRadiusRatio / 3.2)
	 *     cone radius  = 16 uu  * (TracerMuzzleRadiusUU    / 2.5)
	 *
	 * At the shipped defaults both brackets are exactly 1 and the doc's numbers appear on screen
	 * unmodified — which is what a verifier measuring the on-screen size will find. Move a slider and
	 * it still does what its tooltip says (bigger halo, smaller flash); its endpoint behaviour is
	 * preserved too, because a ratio of 1.0 shrinks the halo inside the core and the halo is then
	 * dropped, exactly as "1.0 removes the halo entirely" promises.
	 *
	 * These two constants are therefore NOT tuning values. They are a record of the defaults this
	 * profile was calibrated against, and if either default ever moves in TraceSettings.h and
	 * DefaultGame.ini, the honest fix is to move it here too. Called out in the pass report.
	 */
	static constexpr float HaloRatioReferenceDefault = 3.2f;
	static constexpr float FlashRadiusReferenceDefaultUU = 2.5f;


	/**
	 * The M_TraceNeon "Glow" multiplier at t=0 - how far past white the beam core's emissive sits,
	 * and therefore how hard the shot blooms. THE CORE ONLY: the muzzle cone used to share this
	 * block and no longer belongs in it, because it is no longer drawn on M_TraceNeon at all. See
	 * MuzzleIntensity below.
	 *
	 * Calibrated against the rest of the arena rather than picked in the abstract: the neon grid
	 * runs at 1.5, structural trim at 3.2-4.5, the goal line at 6.0, and the carrier's trail ghost
	 * peaks at 4.2. A railgun should be the hottest thing on the field, so the core sits just above
	 * the goal line - and no higher. An earlier draft ran these at 26 to 34 and every shot was an
	 * undifferentiated white blob that erased the arena behind it.
	 *
	 * CoreIntensity 5.5 -> 6.6 WITH THE THINNING (spec v4 §4). Halving a beam's radius quarters its
	 * cross-sectional area and therefore roughly quarters the light it contributes to the bloom pass,
	 * so a beam made thinner at unchanged brightness reads as *dimmer* as well as thinner - and at
	 * arena distances "dimmer and thinner" is how a tracer becomes one the player cannot follow. A
	 * 20% lift on the emissive is the compensation, and it is deliberately not the full 4x: a thin
	 * bright line is the goal, and pushing an unlit emissive far past the tonemapper's shoulder just
	 * turns it white and takes the team colour with it.
	 *
	 * NOTE, hard won: brightness MUST ride on this scalar. Encoding it in the "Color" vector
	 * parameter instead does not work - a material instance clamps vector parameters to [0,1], so
	 * Color * 34 silently became flat white at intensity 1 and the beam rendered as a dull matte
	 * tube with no bloom at all. That failure looks exactly like "the effect is not rendering".
	 */
	static constexpr float CoreIntensity = 6.6f;

	/**
	 * The muzzle cone's brightness — and it is 1.0, not the 3.0 it was, because the cone moved off
	 * the opaque emissive material and onto the ADDITIVE one (see the blend argument in InitTracer).
	 *
	 * That move changes what this number can mean. On M_TraceNeon brightness rides a "Glow" SCALAR
	 * and can be pushed past white to bloom; the engine's additive material has no such scalar, so
	 * UTraceFxShapes::SetGlow has to fold brightness into the "Color" vector — and a material
	 * instance clamps vector parameters to [0,1]. Leaving this at 3.0 would therefore not have made
	 * the flash three times brighter. It would have PINNED IT AT FLAT WHITE for the whole first 42%
	 * of its life (3 x fade^2 >= 1 while fade >= 0.577) and only then let the fade start, which is
	 * both a hard-edged white disc and the loss of the hot-to-cool falloff the shockwave reading
	 * depends on. 1.0 is the largest value the blend can actually carry, so the fade curve is
	 * expressed in full from the first frame.
	 *
	 * Exactly the same constraint the halo has lived under since v32; HaloIntensity's comment below
	 * carries the same argument for the same reason, and it is 1.0 for the same reason.
	 */
	static constexpr float MuzzleIntensity = 1.0f;

	/**
	 * The halo's material has no Glow scalar, so its intensity has to ride in the colour - which
	 * means it is clamped at 1.0 and cannot be pushed past white. That is fine: the halo's job is a
	 * soft coloured sleeve, not a hot core, and additive blending at full colour is already a strong
	 * glow.
	 *
	 * FULL 1.0 SINCE v32, where the pre-v32 sheath sat at 0.85 "so the fade always has somewhere to
	 * go". The fade now rides on OPACITY, which is a separate argument to UTraceFxShapes::SetGlow and
	 * is the FX doc's own 0.55 — so the brightness no longer has to carry two jobs, and the number a
	 * verifier is told to measure ("the halo opacity") is a number that exists in the code rather
	 * than a product of two constants that were never meant to be multiplied.
	 */
	static constexpr float HaloIntensity = 1.0f;

	/**
	 * How far toward white the core and muzzle are pushed from the team colour.
	 *
	 * Half and half. A hotter core is more convincing as energy, but at 0.78 the beam came out
	 * essentially white and a player could no longer tell whose shot had just gone past them -
	 * which in a team game is information, not decoration. The halo stays fully team coloured.
	 */
	static constexpr float HotColorWhiteMix = 0.50f;

	// FirstPersonStandoffUU / FirstPersonRightOffsetUU / FirstPersonDownOffsetUU ARE GONE (spec v26
	// §4). They were the hand-tuned screen offset that put the beam beside the barrel; the start now
	// comes from ATraceCharacter::GetViewModelMuzzleViewPoint(). See the class comment. They are
	// deleted rather than defaulted to zero, so nothing can quietly start using them again.

	/**
	 * Camera-to-shot-origin distance under which the first person treatment is applied at all.
	 *
	 * A GATE, not an offset, which is why it survived the deletion above. Every machine spawns a
	 * tracer for every shot in the match; this is what distinguishes "the beam I just fired, whose
	 * origin is 22 uu from my eye" from "someone else's beam, 3000 uu away", so that only my own shot
	 * is moved onto my own barrel. Comfortably larger than MuzzleForward (22) and comfortably smaller
	 * than any distance another player can be at.
	 */
	static constexpr float FirstPersonProximityUU = 160.0f;

	/**
	 * How much beam must remain BEYOND the muzzle for the move to be worth making, in uu.
	 *
	 * The muzzle is ~85 uu down the shot, so a point-blank hit on a wall closer than that would put
	 * the start past the impact and draw the beam backwards. The old standoff had the same hazard and
	 * handled it by shrinking the standoff; there is nothing to shrink now that the start is a place
	 * rather than a distance, so the answer is simply to leave the beam at the true origin for shots
	 * too short to relocate. Those are pressed-against-a-wall shots where the beam is a few pixels
	 * long and unreadable either way.
	 */
	static constexpr float MinBeamBeyondMuzzleUU = 40.0f;

	/**
	 * SPEC v30 §5. How close the pawn's muzzle marker must be to the on-screen gun's own muzzle
	 * landmark, in uu, for the marker to be taken as the answer.
	 *
	 * A TEST OF IDENTITY, NOT A TOLERANCE. Either the marker hangs off the gun being drawn — in which
	 * case the two are the same point and the distance is float noise (Trace.Smg.Probe reports
	 * 0.0000 uu on both guns) — or it hangs off something else, in which case it is wrong by the
	 * rig-space gap between two guns' landmarks: the pistol's 107.4 cm against the SMG's 58.8 cm is a
	 * MEASURED 14.58 uu on the shipped rig, and a marker left on the RIG ROOT would be further still.
	 * 1 uu sits far above the noise and an order below the smallest real disagreement, so nothing
	 * lands near it: widen it or narrow it by 10x and the decision does not change.
	 */
	static constexpr double MuzzleAgreementUU = 1.0;

	// BasicShapeExtentUU IS GONE FROM THIS FILE (SPEC v32 §1: "Write the conversion once, in one
	// named constant, and use it everywhere"). It now lives as UTraceFxShapes::RadiusUUToShapeScale
	// and its length twin, which are the only two places in the module that know a BasicShape is
	// 100 uu across. Deleted rather than left as a synonym, so nothing here can drift from it.

	/** Shorter than this and the shot is not worth drawing (also avoids a zero-length rotation). */
	static constexpr float MinTracerLengthUU = 1.0f;

	/** Below this beam length the muzzle flash is skipped: it would sit on top of the impact. */
	static constexpr float MinLengthForMuzzleFlashUU = 60.0f;

	/**
	 * Hard bounds applied to the settings-driven radii after they are read, in uu.
	 *
	 * The UPROPERTY clamps already stop a slider producing these, but Get() reads a CDO that
	 * DefaultGame.ini is layered over, and an ini is not clamp-checked. A zero or negative radius
	 * scales the beam mesh to nothing and looks exactly like "the tracer stopped rendering"; an
	 * enormous one is a wall across the arena on every shot. Both are one typo away, and this effect
	 * spawns up to ~80 times a second, so it is worth two lines of defence.
	 */
	static constexpr float MinSafeRadiusUU = 0.05f;
	static constexpr float MaxSafeRadiusUU = 400.0f;
};
