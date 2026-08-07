#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TraceMeleeArc.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMeshComponent;
class UWorld;

/**
 * The knife slash effect. Purely cosmetic, one actor per swing, self-deleting.
 *
 * --- WHY THIS EXISTS AT ALL -------------------------------------------------------------------
 *
 * Spec v10 §1: "the swing itself should read as a swing (an arc/sweep, not a hitscan point)".
 *
 * Half of that is solved by the first-person viewmodel, which physically swings the blade across
 * the swinger's own screen — see UTraceWeaponComponent's swing animation. But the swinger is not
 * who the requirement is about. The person the knife is being swung AT sees a third-person body,
 * and this arena's third-person characters run ABP_Unarmed with no melee animation in the imported
 * set (the same absence that forced UpdateCrouchPresentation to pose the slide by hand). So without
 * this actor, being knifed looks like taking 100 damage from somebody standing near you: no tell, no
 * direction, no reason. That is the single most likely "the knife is broken" report.
 *
 * --- WHAT IT IS MADE OF -----------------------------------------------------------------------
 *
 * A fan of thin emissive cylinders laid along the chord of the swept arc at the blade tip's radius,
 * built from /Engine/BasicShapes because this project ships no .uasset VFX (build contract §2) —
 * the same constraint and the same solution as ATraceTracer.
 *
 * It animates in two beats over one short life:
 *
 *   1. SWEEP   segments light up in order across the first SweepFraction of the life, so the slash
 *              DRAWS ITSELF from one end of the arc to the other. This is the beat that carries the
 *              direction of the swing, which is the information a victim needs — an arc that
 *              appears all at once is a static decal and reads as a muzzle flash.
 *   2. FADE    every lit segment decays from its own ignition, so the leading edge is brightest and
 *              the tail is already dying. A uniform fade reads as a light being switched off; a
 *              trailing one reads as motion.
 *
 * Nothing here is ever consulted by the hit resolution. TraceMelee::ResolveSwing has already
 * decided everything by the time this is spawned, on the server, and the multicast that spawns it
 * deliberately skips the swinger — who drew their own the instant they pressed, exactly as
 * UTraceWeaponComponent::MulticastFireEffects skips the shooter.
 */
UCLASS()
class TRACE_API ATraceMeleeArc : public AActor
{
	GENERATED_BODY()

public:
	ATraceMeleeArc();

	/**
	 * Spawns one slash. Safe to call on any machine that renders; a no-op on a dedicated server.
	 *
	 * @param World       game world.
	 * @param Origin      blade origin — the same GetMuzzleLocation() the swing was resolved from.
	 * @param Forward     centre of the arc, unit length.
	 * @param SwingAxis   axis the arc is swept about; the arc lies in the plane perpendicular to it.
	 *                    Pass the SAME axis TraceMelee::ResolveSwing derived, or the drawn arc and
	 *                    the resolved one are two different swings.
	 * @param ArcDegrees  total sweep width, centred on Forward.
	 * @param RadiusUU    reach; the drawn arc sits at BladeRadiusFraction of it.
	 * @param Color       team colour of the swinger.
	 * @param bConnected  true when the swing actually hit somebody — the slash flares brighter and
	 *                    lives slightly longer, which is the only feedback a bystander gets that
	 *                    somebody just took a hundred damage.
	 */
	static ATraceMeleeArc* Spawn(UWorld* World, const FVector& Origin, const FVector& Forward,
		const FVector& SwingAxis, float ArcDegrees, float RadiusUU, const FLinearColor& Color,
		bool bConnected);

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	/** Builds the fan. Called once, from Spawn, before the actor is allowed to tick. */
	void Build(const FVector& Forward, const FVector& SwingAxis, float ArcDegrees, float RadiusUU,
		const FLinearColor& Color, bool bConnected);

	/** Pushes Color/Glow onto one segment's MID, degrading gracefully on the fallback material. */
	void PushSegment(int32 Index, float Intensity);

	UPROPERTY()
	TObjectPtr<USceneComponent> Root;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Segments;

	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SegmentMIDs;

	/** M_TraceNeon if the generated content is present, otherwise BasicShapeMaterial. Both optional. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> NeonMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> FallbackMaterial;

	UPROPERTY()
	TObjectPtr<class UStaticMesh> CylinderMesh;

	FLinearColor SlashColor = FLinearColor::White;

	float Age = 0.f;
	float LifeSeconds = 0.22f;
	float PeakGlow = 6.f;

	/** True when the material actually understands Color/Glow, i.e. we are not on the fallback. */
	bool bHaveNeonParameters = false;

	// --- Shape, all arithmetic rather than taste --------------------------------------------------

	/** Chords along the tip path. 9 is smooth enough that the arc does not read as a polygon. */
	static constexpr int32 NumSegments = 9;

	/** Where along the reach the drawn arc sits. The tip is the part of a blade the eye follows. */
	static constexpr float BladeRadiusFraction = 0.78f;

	/** Fraction of the life spent drawing the sweep. The rest is pure decay. */
	static constexpr float SweepFraction = 0.42f;

	/** Thickness of a chord, in uu. Thin: this is a cut, not a beam. */
	static constexpr float SegmentThicknessUU = 3.2f;
};
