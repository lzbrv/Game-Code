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
 * A railgun shot has four beats, and this actor draws all four out of engine primitives, because
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
 *   3. MUZZLE    a short, bright sphere at the near end of the beam.
 *   4. IMPACT    a sphere at the hit point that EXPANDS as it dims - the pop that gives the shot
 *                its weight, and the thing that tells a player at a glance that they connected.
 *                Suppressed when the shot died at maximum range with nothing to hit.
 *
 * The whole thing snaps to full length on frame one (it is hitscan - there is no travel time to
 * animate) and then decays over TracerLifeSeconds with an ease-out: bright and instant, then a
 * short weighty fade. Nothing translates; only intensity and thickness animate.
 *
 * --- THE FIRST PERSON OFFSET, AND WHY THE OLD TRACER WAS INVISIBLE ----------------------------
 *
 * ATraceCharacter::GetMuzzleLocation() sits 22 uu in front of the eye, ON the aim ray, and
 * GetAimDirection() is that same ray. So to the shooter's own camera the beam is a line pointing
 * directly away from the viewer: it projects to a single point behind the crosshair and is, in the
 * strict geometric sense, invisible. That is a large part of why the old tracer "looked odd" - in
 * first person there was nothing to see but the impact.
 *
 * So for the LOCAL shooter only, the near end of the beam is moved to where the gun visibly is:
 * pushed FirstPersonStandoffUU down the shot and offset right and down into the viewmodel's
 * corner of the screen. The beam then runs diagonally from the weapon to the crosshair, which is
 * what every first-person shooter does and what makes the shot legible. Nothing about the
 * gameplay ray changes - this is the last 80 uu of a cosmetic actor, applied on one machine.
 *
 * The standoff is also a SAFETY feature. An unlit emissive sphere 22 uu from the eye subtends
 * about 30 degrees of screen, and unlit emissive is distance-invariant, so it arrives at full
 * intensity however close it is - the exact recipe for the point-blank whiteout this project has
 * already been bitten by once. Everything drawn near the muzzle is pushed out of the near field
 * first, and the impact pop is dimmed and shrunk when it lands close to the camera.
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
	 * @param bImpacted True when the beam actually terminated on a surface, which is what gates the
	 *                  impact pop. Passing false for a shot that died at max range keeps a flash
	 *                  from appearing in mid-air 28000 uu away.
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

	/** Beat 3: muzzle flash. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<UStaticMeshComponent> MuzzleFlash;

	/** Beat 4: expanding impact pop. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<UStaticMeshComponent> ImpactFlash;

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

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ImpactMID;

	/** Team colour of the shot, cached for the per-frame fade. */
	FLinearColor ShotColor = FLinearColor::White;

	/** Near-white version of ShotColor used by the core and the muzzle. */
	FLinearColor HotColor = FLinearColor::White;

	float Age = 0.f;

	/**
	 * 0.3..1, from how close the impact point is to the local camera. Scales the impact pop's size
	 * and brightness down when it goes off in the viewer's face. See InitTracer().
	 */
	float ImpactProximityScale = 1.f;

	/** Beam widths for this particular shot, derived from its length. See CoreDiameterPerLengthUU. */
	float CoreDiameter = CoreDiameterMinUU;
	float SheathDiameter = CoreDiameterMinUU * SheathWidthRatio;

	/** Set false once the muzzle flash has been retired, so it is only hidden once. */
	bool bMuzzleVisible = false;
	bool bImpactVisible = false;

	// --- Tuning ---------------------------------------------------------------------------------
	//
	// These are deliberately compile-time constants rather than config: they are the *look* of one
	// effect, they are meaningless outside this file, and every one of them was chosen against the
	// arena's bloom settings. A settings page for them would be twelve knobs nobody turns.

	/** Total life. Long enough to read as a beam, short enough that 10 bots firing never smears. */
	static constexpr float TracerLifeSeconds = 0.17f;

	/** Fraction of the life over which the muzzle flash exists. */
	static constexpr float MuzzleFlashLifeFraction = 0.42f;

	/**
	 * Beam thickness scales with the length of the shot.
	 *
	 * A fixed-width cylinder is the wrong model for something meant to read at every range in a
	 * 24000 uu arena: 4 uu is a fat rod across a corridor and a sub-pixel thread across the field.
	 * Making the diameter proportional to the shot's own length holds the beam at roughly constant
	 * angular width from the shooter's eye, which is what a real beam material would do with a
	 * camera-facing ribbon. Clamped at both ends so a knife-fight shot is not a needle and a
	 * cross-map shot is not a wall.
	 */
	static constexpr float CoreDiameterPerLengthUU = 0.0062f;
	static constexpr float CoreDiameterMinUU = 5.0f;
	static constexpr float CoreDiameterMaxUU = 26.0f;

	/** The soft glow is this many times the core's diameter. */
	static constexpr float SheathWidthRatio = 3.2f;

	/**
	 * Muzzle flash diameter at t=0. Kept small on purpose: with the standoff below it sits 120 uu
	 * from the shooter's own eye, where 10 uu subtends under 5 degrees. Measured larger and hotter
	 * first, and it bloomed into a blob that ate the middle of the screen on every shot - a
	 * flashbang, not a muzzle flash.
	 */
	static constexpr float MuzzleDiameterUU = 10.0f;

	/** Impact pop diameter, start and end of its expansion. */
	static constexpr float ImpactStartDiameterUU = 10.0f;
	static constexpr float ImpactEndDiameterUU = 52.0f;

	/**
	 * M_TraceNeon "Glow" multipliers at t=0 - how far past white the emissive sits, and therefore
	 * how hard the shot blooms.
	 *
	 * Calibrated against the rest of the arena rather than picked in the abstract: the neon grid
	 * runs at 1.5, structural trim at 3.2-4.5, the goal line at 6.0, and the carrier's trail ghost
	 * peaks at 4.2. A railgun should be the hottest thing on the field, so the core sits just above
	 * the goal line and the impact just above that - and no higher. An earlier draft ran these at 26
	 * to 34 and every shot was an undifferentiated white blob that erased the arena behind it.
	 *
	 * NOTE, hard won: brightness MUST ride on this scalar. Encoding it in the "Color" vector
	 * parameter instead does not work - a material instance clamps vector parameters to [0,1], so
	 * Color * 34 silently became flat white at intensity 1 and the beam rendered as a dull matte
	 * tube with no bloom at all. That failure looks exactly like "the effect is not rendering".
	 */
	static constexpr float CoreIntensity = 5.5f;
	static constexpr float MuzzleIntensity = 3.0f;
	static constexpr float ImpactIntensity = 6.5f;

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

	/** See "THE FIRST PERSON OFFSET" in the class comment. */
	static constexpr float FirstPersonStandoffUU = 120.0f;

	/**
	 * Sideways/downward offset of the beam's near end, in camera space, at the standoff distance.
	 * Chosen so the beam appears to leave the viewmodel in the lower right of the screen: at 120 uu
	 * these subtend about 19 degrees right and 8 degrees down, which lands on the gun at a 95
	 * degree field of view.
	 */
	static constexpr float FirstPersonRightOffsetUU = 42.0f;
	static constexpr float FirstPersonDownOffsetUU = 17.0f;

	/** Camera-to-muzzle distance under which the first person treatment is applied. */
	static constexpr float FirstPersonProximityUU = 160.0f;

	/** /Engine/BasicShapes primitives are 100 uu across, centred on their own origin. */
	static constexpr float BasicShapeExtentUU = 100.0f;

	/** Shorter than this and the shot is not worth drawing (also avoids a zero-length rotation). */
	static constexpr float MinTracerLengthUU = 1.0f;

	/** Below this beam length the muzzle flash is skipped: it would sit on top of the impact. */
	static constexpr float MinLengthForMuzzleFlashUU = 60.0f;
};
