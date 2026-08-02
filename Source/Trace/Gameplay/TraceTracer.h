#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TraceTracer.generated.h"

class UMaterialInterface;
class UStaticMeshComponent;
class UWorld;

/**
 * Purely cosmetic hitscan tracer: an engine BasicShapes cylinder stretched from the muzzle to the
 * impact point, tinted with the shooter's team colour, that deletes itself a few frames later.
 *
 * Never replicated. Every machine that should see a shot spawns its own copy:
 *  - the shooter spawns one the instant it pulls the trigger (predicted, zero latency),
 *  - everyone else spawns one from UTraceWeaponComponent::MulticastFireEffects, which deliberately
 *    skips the shooter so a predicted tracer is never drawn twice.
 */
UCLASS()
class TRACE_API ATraceTracer : public AActor
{
	GENERATED_BODY()

public:
	ATraceTracer();

	/**
	 * Spawns and configures a tracer.
	 * Returns nullptr on a dedicated server (no visuals), for a degenerate/non-finite segment,
	 * or when the world is invalid. Callers may ignore the return value.
	 */
	static ATraceTracer* Spawn(UWorld* World, const FVector& From, const FVector& To, const FLinearColor& Color);

protected:
	UPROPERTY(VisibleAnywhere, Category = "Trace|Tracer")
	TObjectPtr<UStaticMeshComponent> TracerMesh;

	/**
	 * /Engine/BasicShapes/BasicShapeMaterial. Resolved with ConstructorHelpers and held as a hard
	 * UPROPERTY reference on the CDO so the cooker follows it into packaged builds - a bare runtime
	 * LoadObject of an engine asset is not cooked and would resolve to nullptr when shipped.
	 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> TracerMaterial;

private:
	void InitTracer(const FVector& From, const FVector& To, const FLinearColor& Color);

	/** Roughly one frame at 60fps *2 - long enough to read, short enough to never clutter. */
	static constexpr float TracerLifeSeconds = 0.08f;

	/** Beam diameter in unreal units. */
	static constexpr float TracerThicknessUU = 3.0f;

	/** /Engine/BasicShapes/Cylinder is a 100uu tall, 100uu wide cylinder centred on its origin. */
	static constexpr float BasicShapeExtentUU = 100.0f;

	/** Shorter than this and the tracer is not worth spawning (also avoids a zero-length rotation). */
	static constexpr float MinTracerLengthUU = 1.0f;
};
