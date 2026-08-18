#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TraceTracer.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMeshComponent;
class UWorld;

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
 *   1. CORE      a thin, almost white cylinder from muzzle to impact. Near-white rather than team
 *                coloured on purpose - a real energy weapon's centre is hotter than its edges, and
 *                a white core with a coloured sheath is what makes it read as *bright* instead of
 *                merely *blue*. It is emissive far past 1.0, so the scene's bloom does the wide
 *                soft halo for free; that is the same trick every neon surface in this arena uses.
 *   2. SHEATH    a fatter, dimmer, fully team-coloured cylinder around the core, drawn additively
 *                so it cannot occlude the core. Optional: if the additive engine material is
 *                unavailable the effect degrades to core-plus-bloom and still looks deliberate.
 *   3. MUZZLE    a short, bright sphere at the near end of the beam. Suppressible from settings
 *                (UTraceSettings::bTracerMuzzleFlash).
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

protected:
	/** Positioned at the beam start and rotated so local +Z runs down the shot. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<USceneComponent> EffectRoot;

	/** Beat 1: the hot near-white centre of the beam. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<UStaticMeshComponent> BeamCore;

	/** Beat 2: the wider team-coloured additive glow. Hidden if the additive material is missing. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<UStaticMeshComponent> BeamSheath;

	/** Beat 3: muzzle flash. Gated on UTraceSettings::bTracerMuzzleFlash. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<UStaticMeshComponent> MuzzleFlash;

	// ImpactFlash DELETED (spec v4 §4). See "THE FOURTH BEAT, AND WHY IT IS GONE" in the class
	// comment. The component, its MID, its two diameter constants, its intensity, its visibility flag
	// and the camera-proximity dimming that existed solely to stop it whiting out the screen at
	// point-blank range are all gone with it.

	/**
	 * /Game/Generated/Materials/M_TraceNeon — unlit, opaque, EmissiveColor = Color * Glow. Made by
	 * Scripts/generate_content.py; see that file for why no engine material will do. Held as a hard
	 * UPROPERTY on the CDO so the cooker follows it.
	 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> NeonMaterial;

	/**
	 * /Engine/EngineMaterials/EmissiveMeshMaterial — unlit and BLEND_Additive, which is the one
	 * property the sheath actually needs: additive geometry writes no depth, so a fat sheath cannot
	 * hide the thin core inside it. Its emissive is modulated by a faint grid texture, which is why
	 * it is used ONLY for the soft outer glow, where a texture is invisible under bloom, and never
	 * for the core.
	 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> AdditiveMaterial;

	/** Fallback when M_TraceNeon has not been generated: lit, colour only, no emissive. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> FallbackMaterial;

private:
	void InitTracer(const FVector& From, const FVector& To, const FLinearColor& Color, bool bImpacted);

	/** Applies the intensity/thickness curves for a normalised age in [0,1]. */
	void ApplyFade(float Alpha);

	/** Creates a MID on Mesh from Material and stores it. Null-safe; returns the MID or nullptr. */
	UMaterialInstanceDynamic* MakeMID(UStaticMeshComponent* Mesh, UMaterialInterface* Material);

	/**
	 * Pushes colour and brightness into a MID.
	 *
	 * @param bGlowScalar true for M_TraceNeon and the BasicShapeMaterial fallback, whose contract is
	 *                    "Color is a 0..1 hue, Glow is the multiplier" — the same idiom the arena
	 *                    builder and the trail already use on this material. False for the additive
	 *                    engine material, which has no Glow scalar, so intensity has to be baked
	 *                    into the colour instead.
	 */
	static void SetMIDColor(UMaterialInstanceDynamic* MID, const FLinearColor& Color, float Intensity, bool bGlowScalar);

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CoreMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SheathMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MuzzleMID;

	/** Team colour of the shot, cached for the per-frame fade. */
	FLinearColor ShotColor = FLinearColor::White;

	/** Near-white version of ShotColor used by the core and the muzzle. */
	FLinearColor HotColor = FLinearColor::White;

	float Age = 0.f;

	/**
	 * Beam DIAMETERS for this particular shot. All three are overwritten by InitTracer from the
	 * Tracer block in UTraceSettings before anything is drawn; the initialisers here only keep a
	 * tracer that returned early from InitTracer (a degenerate segment) from holding garbage.
	 *
	 * DIAMETERS rather than radii because /Engine/BasicShapes/Cylinder and Sphere are 100 uu ACROSS,
	 * so a diameter is what divides cleanly into a mesh scale. The settings are expressed as RADII
	 * because that is the word spec v4 §4 uses ("the radius of the cross section"); InitTracer
	 * doubles them exactly once, at the boundary, so the rest of this file stays in one unit.
	 */
	float CoreDiameter = 4.f;
	float SheathDiameter = 12.f;
	float MuzzleDiameter = 5.f;

	/** Set false once the muzzle flash has been retired, so it is only hidden once. */
	bool bMuzzleVisible = false;

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

	/** Total life. Long enough to read as a beam, short enough that 10 bots firing never smears. */
	static constexpr float TracerLifeSeconds = 0.17f;

	/** Fraction of the life over which the muzzle flash exists. */
	static constexpr float MuzzleFlashLifeFraction = 0.42f;

	/**
	 * M_TraceNeon "Glow" multipliers at t=0 - how far past white the emissive sits, and therefore
	 * how hard the shot blooms.
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
	static constexpr float MuzzleIntensity = 3.0f;

	/**
	 * The additive sheath's engine material has no Glow scalar, so its intensity has to ride in the
	 * colour - which means it is clamped at 1.0 and cannot be pushed past white. That is fine: the
	 * sheath's job is a soft coloured halo, not a hot core, and additive blending at full colour is
	 * already a strong glow. Kept just under 1 so the fade always has somewhere to go.
	 */
	static constexpr float SheathIntensity = 0.85f;

	/**
	 * How far toward white the core and muzzle are pushed from the team colour.
	 *
	 * Half and half. A hotter core is more convincing as energy, but at 0.78 the beam came out
	 * essentially white and a player could no longer tell whose shot had just gone past them -
	 * which in a team game is information, not decoration. The sheath stays fully team coloured.
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

	/** /Engine/BasicShapes primitives are 100 uu across, centred on their own origin. */
	static constexpr float BasicShapeExtentUU = 100.0f;

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
