// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ATR_EchoObstacleBehaviorDataAsset.generated.h"

// Per-ECHO obstacle interaction capabilities (what this Echo archetype is willing/able to
// do to barriers). Consumed by barrier engagement: capability bools gate damage by barrier
// type, ObstacleDamagePerHit overrides the settings fallback, and GroupPoundingAgitationAmount
// is deposited while a group pounds the same obstacle. Per-BARRIER interaction rules live in
// UATR_EchoBarrierDataAsset (returned by the barrier actor's IATR_EchoBarrier interface).
// Referenced from UATR_EchoSettings (default) and overridable per archetype.
UCLASS(BlueprintType, meta = (DisplayName = "Echo Obstacle Behavior"))
class ALLTHATREMAINS_API UATR_EchoObstacleBehaviorDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	// Whether this Echo archetype damages doors/barricades when engaging them. False = press only.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle")
	bool bCanAttackDoors = false;

	// Whether this Echo archetype damages windows when engaging them. False = press only.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle")
	bool bCanBreakWindows = false;

	// Wind-up before the first hit after reaching barrier contact.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle", meta = (ClampMin = "0.0", ForceUnits = "s"))
	float AttackObstacleDelaySeconds = 0.5f;

	// Per-hit barrier damage for this Echo archetype (before group-pressure scaling).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle", meta = (ClampMin = "0.0"))
	float ObstacleDamagePerHit = 10.f;

	// Agitation deposited when a group is pounding the same obstacle — draws more Echoes in.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle", meta = (ClampMin = "0.0"))
	float GroupPoundingAgitationAmount = 0.2f;
};
